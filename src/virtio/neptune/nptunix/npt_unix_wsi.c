/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * X11 DRI3/Present WSI for nptunix.so.
 *
 * Path 1 (DRI3 >= 1.4 explicit sync): wrap the per-Present sync_file
 * in a syncobj, xcb_dri3_import_syncobj it into the X server, and
 * submit via xcb_present_pixmap_synced.  X waits in its present queue
 * with no host stall.
 *
 * Path 2 (older DRI3): drop the fence and submit with
 * wait_fence=XCB_NONE.  Cold-start tear risk is negligible since
 * blit-to-dmabuf takes microseconds.
 *
 * Both paths consume the fence fd and report wait_fence_consumed=1.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

#include <xf86drm.h>

#include "npt_unix_internal.h"
#include "npt_unixlib.h"

/* One per swapchain: its own X connection, window, pixmaps and Present
 * event queue, so swapchains never see each other's images or events. */
struct npt_unix_wsi {
   xcb_connection_t *conn;
   xcb_window_t window;
   uint32_t width, height;
   uint32_t num_images;
   struct {
      xcb_pixmap_t pixmap;
      int fd;
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
      /* Persistent X XID, rebound each Present. */
      xcb_dri3_syncobj_t acquire_syncobj;
#endif
   } images[NPT_WSI_MAX_IMAGES];
   uint32_t serial;
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   bool dri3_has_explicit_sync;
   int drm_fd;
#endif
   xcb_present_event_t present_eid;
   xcb_special_event_t *special_event;
};

static struct npt_unix_wsi *
wsi_from_handle(uint64_t handle)
{
   return (struct npt_unix_wsi *)(uintptr_t)handle;
}

static void
wsi_free(struct npt_unix_wsi *w)
{
   if (w->conn) {
      for (uint32_t i = 0; i < w->num_images; i++) {
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
         if (w->images[i].acquire_syncobj != XCB_NONE)
            xcb_dri3_free_syncobj(w->conn, w->images[i].acquire_syncobj);
#endif
         if (w->images[i].pixmap)
            xcb_free_pixmap(w->conn, w->images[i].pixmap);
      }
      if (w->special_event)
         xcb_unregister_for_special_event(w->conn, w->special_event);
      xcb_disconnect(w->conn);
   }
   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++) {
      if (w->images[i].fd >= 0)
         close(w->images[i].fd);
   }
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   if (w->drm_fd >= 0)
      close(w->drm_fd);
#endif
   free(w);
}

NTSTATUS npt_do_wsi_init(void *args)
{
   struct npt_unix_wsi_init_params *p = args;
   p->result = -1;
   p->wsi = 0;

   if (!p->num_images || p->num_images > NPT_WSI_MAX_IMAGES)
      return STATUS_SUCCESS;

   struct npt_unix_wsi *w = calloc(1, sizeof(*w));
   if (!w)
      return STATUS_SUCCESS;
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   w->drm_fd = -1;
#endif
   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++) {
      w->images[i].fd = -1;
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
      w->images[i].acquire_syncobj = XCB_NONE;
#endif
   }

   w->conn = xcb_connect(NULL, NULL);
   if (xcb_connection_has_error(w->conn)) {
      fprintf(stderr, "neptune unixlib wsi: failed to connect to X\n");
      wsi_free(w);
      return STATUS_SUCCESS;
   }

   /* DRI3 >= 1.4 unlocks path 1. */
   xcb_dri3_query_version_reply_t *dri3 = xcb_dri3_query_version_reply(
      w->conn, xcb_dri3_query_version(w->conn, 1, 4), NULL);
   if (!dri3) {
      fprintf(stderr, "neptune unixlib wsi: DRI3 not available\n");
      wsi_free(w);
      return STATUS_SUCCESS;
   }
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   w->dri3_has_explicit_sync = (dri3->major_version > 1) ||
                               (dri3->major_version == 1 && dri3->minor_version >= 4);
#endif
   free(dri3);

#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   /* Need a render node to make syncobjs from sync_files. */
   if (w->dri3_has_explicit_sync) {
      w->drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
      if (w->drm_fd < 0) {
         fprintf(stderr, "neptune unixlib wsi: open(/dev/dri/renderD128) failed (%s); "
                 "falling back to no-wait-fence path\n", strerror(errno));
         w->dri3_has_explicit_sync = false;
      }
   }
#endif

   xcb_present_query_version_reply_t *pres = xcb_present_query_version_reply(
      w->conn, xcb_present_query_version(w->conn, 1, 2), NULL);
   if (!pres) {
      fprintf(stderr, "neptune unixlib wsi: Present not available\n");
      wsi_free(w);
      return STATUS_SUCCESS;
   }
   free(pres);

   w->window = p->x11_window;
   if (!w->window) {
      fprintf(stderr, "neptune unixlib wsi: no X11 window supplied\n");
      wsi_free(w);
      return STATUS_SUCCESS;
   }
   w->width = p->width;
   w->height = p->height;
   w->num_images = p->num_images;

   for (uint32_t i = 0; i < p->num_images; i++) {
      if (p->fds[i] < 0) continue;

      w->images[i].fd = dup(p->fds[i]);
      w->images[i].pixmap = xcb_generate_id(w->conn);

      /* PixmapFromBuffer takes ownership of the fd via SCM_RIGHTS, so
       * dup again so our stored fd survives for the buffer lifetime. */
      int xcb_fd = dup(w->images[i].fd);
      xcb_void_cookie_t cookie = xcb_dri3_pixmap_from_buffer_checked(
         w->conn, w->images[i].pixmap, w->window,
         p->strides[i] * p->height,
         p->width, p->height, p->strides[i],
         24, 32, xcb_fd);

      xcb_generic_error_t *err = xcb_request_check(w->conn, cookie);
      if (err) {
         fprintf(stderr, "neptune unixlib wsi: DRI3 PixmapFromBuffer failed for image %u (error %d)\n",
                 i, err->error_code);
         free(err);
         w->images[i].pixmap = 0;
         continue;
      }
      fprintf(stderr, "neptune unixlib wsi: pixmap 0x%x for image %u (%ux%u)\n",
              w->images[i].pixmap, i, p->width, p->height);

#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
      if (w->dri3_has_explicit_sync) {
         /* XID is rebound to a fresh sync_file each Present. */
         w->images[i].acquire_syncobj = xcb_generate_id(w->conn);
      }
#endif
   }

   w->present_eid = xcb_generate_id(w->conn);
   xcb_present_select_input(w->conn, w->present_eid, w->window,
                            XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                            XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
   w->special_event = xcb_register_for_special_xge(
      w->conn, &xcb_present_id, w->present_eid, NULL);
   xcb_flush(w->conn);

#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   const int explicit_sync_log = (int)w->dri3_has_explicit_sync;
#else
   const int explicit_sync_log = 0;
#endif
   fprintf(stderr, "neptune unixlib wsi: initialized for window 0x%x "
                   "(%u images, %ux%u, explicit_sync=%d)\n",
           w->window, p->num_images, p->width, p->height,
           explicit_sync_log);
   p->wsi = (uint64_t)(uintptr_t)w;
   p->result = 0;
   return STATUS_SUCCESS;
}

static int
image_index_for_pixmap(struct npt_unix_wsi *w, xcb_pixmap_t pixmap)
{
   for (uint32_t i = 0; i < w->num_images; i++) {
      if (w->images[i].pixmap == pixmap)
         return (int)i;
   }
   return -1;
}

/* Per-image counts (NOT a bitmask): pending_presents = presents -
 * releases on the host, and a single decrement can't undo multiple
 * lost increments. */
static void
drain_idle_notify(struct npt_unix_wsi *w, uint32_t *out_counts)
{
   xcb_generic_event_t *ev;
   while ((ev = xcb_poll_for_special_event(w->conn, w->special_event))) {
      xcb_present_generic_event_t *pe = (xcb_present_generic_event_t *)ev;
      if (pe->evtype == XCB_PRESENT_EVENT_IDLE_NOTIFY) {
         xcb_present_idle_notify_event_t *ie =
            (xcb_present_idle_notify_event_t *)ev;
         int idx = image_index_for_pixmap(w, ie->pixmap);
         if (idx >= 0 && (uint32_t)idx < NPT_WSI_MAX_IMAGES)
            out_counts[idx]++;
      }
      free(ev);
   }
}

#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
/* Caller still owns sync_file_fd; returned fd is consumed by
 * xcb_dri3_import_syncobj. */
static int
sync_file_to_syncobj_fd(struct npt_unix_wsi *w, int sync_file_fd)
{
   uint32_t handle = 0;
   if (drmSyncobjCreate(w->drm_fd, 0, &handle) != 0)
      return -1;
   if (drmSyncobjImportSyncFile(w->drm_fd, handle, sync_file_fd) != 0) {
      drmSyncobjDestroy(w->drm_fd, handle);
      return -1;
   }
   int syncobj_fd = -1;
   if (drmSyncobjHandleToFD(w->drm_fd, handle, &syncobj_fd) != 0) {
      drmSyncobjDestroy(w->drm_fd, handle);
      return -1;
   }
   /* Destroy the handle; the exported fd keeps the syncobj alive. */
   drmSyncobjDestroy(w->drm_fd, handle);
   return syncobj_fd;
}
#endif /* NPT_HAVE_XCB_DRI3_SYNCOBJ */

/*
 * ASYNC: Mutter doesn't deliver vblanks for composited windows, so
 *        the default Present mode wedges after a few frames.
 * COPY:  copy into backing store instead of page-flipping.
 */
static const uint32_t kPresentOptions =
   XCB_PRESENT_OPTION_ASYNC | XCB_PRESENT_OPTION_COPY;

NTSTATUS npt_do_wsi_present(void *args)
{
   struct npt_unix_wsi_present_params *p = args;
   struct npt_unix_wsi *w = wsi_from_handle(p->wsi);
   p->result = -1;
   p->wait_fence_consumed = 0;

   if (!w || p->image_index >= w->num_images)
      return STATUS_SUCCESS;

   xcb_pixmap_t pixmap = w->images[p->image_index].pixmap;
   if (!pixmap) return STATUS_SUCCESS;

   const uint32_t this_serial = ++w->serial;

#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   if (w->dri3_has_explicit_sync &&
       w->images[p->image_index].acquire_syncobj != XCB_NONE) {
      xcb_dri3_syncobj_t acquire_xid =
         w->images[p->image_index].acquire_syncobj;

      if (p->wait_fence_fd >= 0) {
         int syncobj_fd = sync_file_to_syncobj_fd(w, p->wait_fence_fd);
         /* libdrm doesn't take ownership of imported sync_files. */
         close(p->wait_fence_fd);
         p->wait_fence_consumed = 1;
         if (syncobj_fd >= 0) {
            /* import_syncobj rebinds acquire_xid; SCM_RIGHTS the fd. */
            xcb_dri3_import_syncobj(w->conn, acquire_xid,
                                    w->window, syncobj_fd);
         } else {
            fprintf(stderr, "neptune unixlib wsi: sync_file -> syncobj failed for image %u\n",
                    p->image_index);
            /* Fall through: acquire_xid still has the previous (signaled)
             * syncobj, so the X server won't block. */
         }
      }

      /* Binary syncobj: acquire_point/release_point=0.  Release
       * syncobj is XCB_NONE since IDLE_NOTIFY + IMAGE_RELEASE
       * already covers the host-side release path. */
      xcb_present_pixmap_synced(w->conn, w->window, pixmap,
                                this_serial, 0, 0, 0, 0,
                                XCB_NONE,
                                acquire_xid, XCB_NONE,
                                /*acquire_point=*/0, /*release_point=*/0,
                                kPresentOptions,
                                0, 0, 0, 0, NULL);
      xcb_flush(w->conn);
      p->result = 0;
      return STATUS_SUCCESS;
   }
#endif /* NPT_HAVE_XCB_DRI3_SYNCOBJ */

   /* Path 2 (libxcb < 1.16): no DRI3-1.4 syncobj path available, so we
    * can't pass the wait fence to xcb_present_pixmap.  Instead, block
    * here on the sync_file ourselves: the X server's copy must not
    * start until the host GPU has finished writing the dmabuf, and
    * cross-VM implicit dmabuf sync doesn't propagate through virtio-gpu
    * blob.
    *
    * 30 s cap: defensive only.  A heavy frame can legitimately take
    * tens of ms; releasing the handshake too early causes the X server
    * to copy a partially-written dmabuf.  Bounded above by the host's
    * own retire-as-device-lost backstop. */
   if (p->wait_fence_fd >= 0) {
      struct pollfd pfd = { .fd = p->wait_fence_fd, .events = POLLIN };
      int rc;
      do {
         rc = poll(&pfd, 1, 30000);
      } while (rc < 0 && errno == EINTR);
      close(p->wait_fence_fd);
      p->wait_fence_consumed = 1;
   }

   xcb_present_pixmap(w->conn, w->window, pixmap,
                      this_serial, 0, 0, 0, 0,
                      XCB_NONE, XCB_NONE, XCB_NONE,
                      kPresentOptions,
                      0, 0, 0, 0, NULL);
   xcb_flush(w->conn);

   p->result = 0;
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_wsi_drain(void *args)
{
   struct npt_unix_wsi_drain_params *p = args;
   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++)
      p->released_count[i] = 0;
   struct npt_unix_wsi *w = wsi_from_handle(p->wsi);
   p->result = -1;

   if (!w || !w->special_event)
      return STATUS_SUCCESS;

   if (xcb_connection_has_error(w->conn))
      return STATUS_SUCCESS;

   /* Steady-state path; poll(2) below covers cold-start. */
   drain_idle_notify(w, p->released_count);
   bool any = false;
   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++)
      if (p->released_count[i]) { any = true; break; }
   if (any) {
      p->result = 0;
      return STATUS_SUCCESS;
   }

   const int conn_fd = xcb_get_file_descriptor(w->conn);
   if (conn_fd < 0) return STATUS_SUCCESS;

   struct pollfd pfd = { .fd = conn_fd, .events = POLLIN };
   const int rc = poll(&pfd, 1, (int)p->timeout_ms);
   if (rc < 0) {
      if (errno != EINTR && errno != EAGAIN)
         p->result = -2;
      return STATUS_SUCCESS;
   }
   drain_idle_notify(w, p->released_count);
   p->result = 0;
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_wsi_destroy(void *args)
{
   struct npt_unix_wsi_destroy_params *p = args;
   struct npt_unix_wsi *w = wsi_from_handle(p->wsi);
   if (w)
      wsi_free(w);
   return STATUS_SUCCESS;
}
