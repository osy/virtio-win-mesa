/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Renderer backend abstraction (virtgpu DRM / vtest / ...).  Shmem
 * regions are refcounted so per-call reply windows survive across
 * ring submissions.
 */

#ifndef NPT_RENDERER_H
#define NPT_RENDERER_H

#include "npt_common.h"
#include "npt_display.h"
#include "nptunix/npt_unixlib.h"

#ifndef _WIN32
#include <unistd.h>
#endif

struct npt_renderer;

struct npt_refcount {
   atomic_int count;
};

static inline void
npt_refcount_init(struct npt_refcount *ref)
{
   atomic_init(&ref->count, 1);
}

static inline void
npt_refcount_inc(struct npt_refcount *ref)
{
   atomic_fetch_add_explicit(&ref->count, 1, memory_order_relaxed);
}

/* True at 0; caller destroys. */
static inline bool
npt_refcount_dec(struct npt_refcount *ref)
{
   int old = atomic_fetch_sub_explicit(&ref->count, 1, memory_order_release);
   assert(old >= 1);
   if (old == 1)
      atomic_thread_fence(memory_order_acquire);
   return old == 1;
}

struct npt_renderer_shmem {
   struct npt_refcount refcount;
   uint32_t res_id;
   size_t size;
   void *mmap_ptr;
   /* The backend can return this shmem's CPU window to its store and
    * re-establish it later (shmem_ops.window_release/window_restore).
    * Set only where releasing the window actually recovers store
    * capacity: the WDDM2 KMD's BAR windows.  Guest-side unmap of a
    * memfd (vtest), a DRM vram mapping (wine virtgpu, whose kernel
    * frees the hostmem carveout only at object destroy), or a VidMm
    * lock (WDDM 1.3) frees nothing, so those windows are permanent and
    * the flag stays clear. */
   bool window_reclaimable;
};

struct npt_renderer_info {
   uint32_t wire_format_version;
   /* NPT_CAPSET_CAP_* bits from the host capset (vtest: all set --
    * it's a dev transport talking to a full renderer). */
   uint32_t caps_flags;
   uint32_t max_timeline_count;
   /* Virtio context id of this transport's context (0 when the
    * backend has no KMD-visible context).  Lets a WDDM device target
    * this context for cross-device submits (flip presents). */
   uint32_t virtio_ctx_id;
};

/* Timeline partition derived from the negotiated max_timeline_count.
 * Slot 0 is the CPU timeline; the upper half is Win32 event proxies
 * (round-robin).  [1, event_ring_base) is reserved for per-ring present
 * fences; the split keeps the host/guest ring-range ABI stable. */
static inline uint32_t
npt_renderer_event_ring_base(const struct npt_renderer_info *info)
{
   return info->max_timeline_count >= 4 ? info->max_timeline_count / 2 : 1;
}

static inline uint32_t
npt_renderer_event_ring_end(const struct npt_renderer_info *info)
{
   return info->max_timeline_count;
}

struct npt_renderer_shmem_ops {
   struct npt_renderer_shmem *(*create)(struct npt_renderer *renderer,
                                        size_t size);
   void (*destroy)(struct npt_renderer *renderer,
                   struct npt_renderer_shmem *shmem);

   /* Occupancy of the store shmems are carved from (the hostmem BAR on
    * virtgpu-win32).  false = no accounting available. */
   bool (*info)(struct npt_renderer *renderer, uint64_t *out_total,
                uint64_t *out_used);

   /* Release / re-establish a shmem's CPU window while the blob and the
    * host memory behind it stay untouched (the host GPU reaches the blob
    * through its own mapping, never the window).  Legal only for shmems
    * marked window_reclaimable and only while nothing reads or writes
    * mmap_ptr: release NULLs mmap_ptr and returns the window to the
    * store; restore repoints mmap_ptr, possibly at a DIFFERENT address.
    * Renderers whose create never sets window_reclaimable leave the ops
    * NULL and every window is permanent. */
   bool (*window_release)(struct npt_renderer *renderer,
                          struct npt_renderer_shmem *shmem);
   bool (*window_restore)(struct npt_renderer *renderer,
                          struct npt_renderer_shmem *shmem);
};

struct npt_renderer_ops {
   void (*destroy)(struct npt_renderer *renderer);

   /* Fire-and-forget on virtgpu (returns once descriptor is queued). */
   bool (*submit_cmd)(struct npt_renderer *renderer,
                      const void *data, size_t size);

   /*
    * Blocks until the host has finished processing.  vtest: aliased
    * to submit_cmd (TCP recv blocks anyway).  virtgpu: submits with
    * EXECBUF_FENCE_FD_OUT and poll(2)s the returned fd.
    */
   bool (*submit_cmd_sync)(struct npt_renderer *renderer,
                           const void *data, size_t size);

   /* Returns dmabuf FD or -1.  Used to import host-created resources. */
   int (*create_host_blob)(struct npt_renderer *renderer,
                           uint64_t blob_id, uint64_t size);

   /* Attach an existing VM-global virtio resource (a shared texture's
    * blob, created by another process/device) to THIS TRANSPORT
    * CONTEXT, so the host worker receives the resource's dmabuf via
    * the proxy's attach-forwarding before a SHARED_OPEN_RES names it.
    * Venus analog: dma-buf import (PRIME fd -> bo -> ATTACH).
    * Implemented as a VIOGPU_RESOURCE_TYPE_IMPORT allocation on the
    * transport device; out_alloc/out_res_kmt are opaque WDDM handles
    * for release_import_res at teardown.  DDI/Win32 only. */
   bool (*import_res)(struct npt_renderer *renderer, uint32_t res_id,
                      uint64_t size,
                      uint32_t *out_alloc, uint32_t *out_res_kmt);
   void (*release_import_res)(struct npt_renderer *renderer,
                              uint32_t alloc, uint32_t res_kmt);

   /* Implementation takes ownership (dups) the FDs.
    * virgl_format is an enum virgl_formats value; backends translate
    * to their target namespace (DRM fourcc, Vulkan format, ...).
    * hwnd is the swapchain's OutputWindow that the backend resolves to
    * its native window handle (Wine: X11 Window via winex11.drv's
    * __wine_x11_whole_window property).  Init fails if hwnd is 0 or
    * has no native backing. */
   bool (*wsi_init)(struct npt_renderer *renderer,
                    uint32_t num_images,
                    const int *fds,
                    const uint32_t *strides,
                    uint32_t virgl_format,
                    uint32_t width, uint32_t height,
                    uint64_t hwnd);

   /*
    * wait_fence_fd: sync_file (or -1) signalling host GPU done for
    * this Present.  Passed to xcb_present_pixmap as wait_fence
    * (DRI3 >= 1.2); on older DRI3 the unixlib polls it before
    * presenting.  Unixlib owns the fd and must close it.
    */
   void (*wsi_present)(struct npt_renderer *renderer,
                       uint32_t image_index,
                       int wait_fence_fd);

   void (*wsi_destroy)(struct npt_renderer *renderer);

   /*
    * Per-image release COUNTS (NOT a bitmask): a fast client can
    * idle_notify the same image multiple times between drains, and
    * a bitmask would collapse them so the host's pending_presents
    * counter never drops to zero.  Must run on a dedicated forwarder
    * thread, not the calling app's render thread (which blocks in the
    * host library's frame-latency sync; only an off-thread drain
    * can unwind onAcquireImage backpressure).
    */
   int (*wsi_drain)(struct npt_renderer *renderer,
                    uint32_t timeout_ms,
                    uint32_t out_released_count[NPT_WSI_MAX_IMAGES]);

   /*
    * Returns a sync_file fd (caller-owned) that signals when the
    * host's sync-queue worker for ring_idx retires the next fence.
    * -1 if the backend doesn't support fence-FD passing.
    */
   int (*submit_present_fence)(struct npt_renderer *renderer,
                               uint32_t ring_idx);

   /*
    * Like submit_present_fence, but additionally asks the transport to
    * signal `direct_event` (a Win32 event HANDLE) itself at completion
    * -- on Windows the KMD KeSetEvents it straight from the completion
    * DPC, sparing the app the waiter-thread hop.  *direct_ok reports
    * whether the transport took over signalling; when false the caller
    * must signal the event itself (Wine/vtest, old KMD,
    * unreferenceable handle).  Optional: absent falls back to
    * submit_present_fence with *direct_ok = false.
    */
   int (*submit_present_fence_direct)(struct npt_renderer *renderer,
                                      uint32_t ring_idx,
                                      void *direct_event, bool *direct_ok);

   /*
    * Monitored-fence gate (Windows KMD only): submit an empty fenced
    * SUBMIT_3D on ring_idx whose retirement fires a KMD-internal token
    * instead of a UM event (VIOGPU_ARM_GATE).  Returns the token (the
    * D3D12 DDI embeds it in a VIOGPU_CMD_GATE DMA packet on the queue's
    * kernel context), 0 on failure/unsupported.
    */
   uint64_t (*arm_gate_fence)(struct npt_renderer *renderer,
                              uint32_t ring_idx);
};

struct npt_renderer {
   struct npt_renderer_info info;
   struct npt_renderer_ops ops;
   struct npt_renderer_shmem_ops shmem_ops;

   /* Releases CPU windows nothing has mapped (npt_d3d12_heap.c registers
    * its trimmer) so a full store can serve a new allocation.  Returns
    * the bytes returned to the store.  MUST NOT enter the renderer's
    * submission lock: it runs under it when a transport allocation
    * retries (see npt_renderer_shmem_create). */
   uint64_t (*shmem_trim)(struct npt_renderer *renderer, uint64_t want);

   /* Set (never cleared) when a ring blob CPU mapping is observed torn
    * down in place -- a guest WDDM TDR removes the whole D3DKMT device
    * and every blob mapping of it reads as the zero page from then on.
    * Every ring wait loop bails on it instead of spinning forever on
    * zeros; submits fail; the runtime's own device-removed handling
    * tears the device down. */
   _Atomic uint32_t lost;
};

static inline bool
npt_renderer_is_lost(struct npt_renderer *renderer)
{
   return atomic_load_explicit(&renderer->lost, memory_order_relaxed) != 0;
}

static inline void
npt_renderer_set_lost(struct npt_renderer *renderer)
{
   atomic_store_explicit(&renderer->lost, 1, memory_order_relaxed);
}

struct npt_renderer *
npt_renderer_create_vtest(void);

struct npt_renderer *
npt_renderer_create_virtgpu(void);

/* Adapter-scope capability probe (win32 virtgpu only).  The D3D11 UMD's
 * GetSupportedVersions runs before any device -- and therefore before the
 * transport -- exists, so the advertised DDI versions come from a
 * standalone one-shot D3DKMT probe of the KMD mode and host capset,
 * latched process-wide. */
struct npt_adapter_probe {
   /* Adapter is viogpu3d with 3D + shmem + the Neptune capset. */
   bool viogpu;
   /* KMD runs in WDDM2 (GpuMmu) mode. */
   bool wddm2;
   /* Host capset fetched and wire format matches. */
   bool host_ok;
   /* NPT_CAPSET_CAP_* bits from the host capset. */
   uint32_t caps_flags;
};

const struct npt_adapter_probe *
npt_adapter_probe(void);

static inline void
npt_renderer_destroy(struct npt_renderer *renderer)
{
   renderer->ops.destroy(renderer);
}

static inline bool
npt_renderer_submit_cmd(struct npt_renderer *renderer,
                        const void *data, size_t size)
{
   return renderer->ops.submit_cmd(renderer, data, size);
}

static inline bool
npt_renderer_submit_cmd_sync(struct npt_renderer *renderer,
                             const void *data, size_t size)
{
   return renderer->ops.submit_cmd_sync(renderer, data, size);
}

static inline struct npt_renderer_shmem *
npt_renderer_shmem_create(struct npt_renderer *renderer, size_t size)
{
   struct npt_renderer_shmem *s = renderer->shmem_ops.create(renderer, size);
   if (s != NULL || renderer->shmem_trim == NULL)
      return s;
   /* The store ran dry: reclaim unmapped heap windows and retry.  A
    * second pass releases everything releasable -- the first pass frees
    * `size` bytes but not necessarily `size` CONTIGUOUS bytes. */
   if (renderer->shmem_trim(renderer, size) > 0)
      s = renderer->shmem_ops.create(renderer, size);
   if (s == NULL && renderer->shmem_trim(renderer, UINT64_MAX) > 0)
      s = renderer->shmem_ops.create(renderer, size);
   return s;
}

static inline bool
npt_renderer_shmem_info(struct npt_renderer *renderer, uint64_t *out_total,
                        uint64_t *out_used)
{
   return renderer->shmem_ops.info != NULL &&
          renderer->shmem_ops.info(renderer, out_total, out_used);
}

static inline bool
npt_renderer_shmem_window_release(struct npt_renderer *renderer,
                                  struct npt_renderer_shmem *shmem)
{
   return renderer->shmem_ops.window_release != NULL &&
          renderer->shmem_ops.window_release(renderer, shmem);
}

static inline bool
npt_renderer_shmem_window_restore(struct npt_renderer *renderer,
                                  struct npt_renderer_shmem *shmem)
{
   return renderer->shmem_ops.window_restore != NULL &&
          renderer->shmem_ops.window_restore(renderer, shmem);
}

static inline void
npt_renderer_shmem_destroy(struct npt_renderer *renderer,
                           struct npt_renderer_shmem *shmem)
{
   renderer->shmem_ops.destroy(renderer, shmem);
}

static inline struct npt_renderer_shmem *
npt_renderer_shmem_ref(struct npt_renderer *renderer,
                       struct npt_renderer_shmem *shmem)
{
   (void)renderer;
   npt_refcount_inc(&shmem->refcount);
   return shmem;
}

static inline void
npt_renderer_shmem_unref(struct npt_renderer *renderer,
                         struct npt_renderer_shmem *shmem)
{
   if (npt_refcount_dec(&shmem->refcount))
      npt_renderer_shmem_destroy(renderer, shmem);
}

/* Drops a reference without waiting for the transport teardown: the last
 * unref hands the shmem to the reaper thread (npt_resource.c), which
 * destroys queued shmems in batches.  For shmem whose host-side user is
 * released asynchronously anyway (D3D12 heaps: COM_RELEASE is queued
 * first and the host keeps the import alive until it lands). */
void
npt_renderer_shmem_unref_async(struct npt_renderer *renderer,
                               struct npt_renderer_shmem *shmem);

static inline int
npt_renderer_create_host_blob(struct npt_renderer *renderer,
                              uint64_t blob_id, uint64_t size)
{
   if (renderer->ops.create_host_blob)
      return renderer->ops.create_host_blob(renderer, blob_id, size);
   return -1;
}

static inline bool
npt_renderer_import_res(struct npt_renderer *renderer, uint32_t res_id,
                        uint64_t size,
                        uint32_t *out_alloc, uint32_t *out_res_kmt)
{
   if (renderer && renderer->ops.import_res)
      return renderer->ops.import_res(renderer, res_id, size,
                                      out_alloc, out_res_kmt);
   return false;
}

static inline void
npt_renderer_release_import_res(struct npt_renderer *renderer,
                                uint32_t alloc, uint32_t res_kmt)
{
   if (renderer && renderer->ops.release_import_res)
      renderer->ops.release_import_res(renderer, alloc, res_kmt);
}

static inline bool
npt_renderer_wsi_init(struct npt_renderer *renderer,
                      uint32_t num_images,
                      const int *fds,
                      const uint32_t *strides,
                      uint32_t virgl_format,
                      uint32_t width, uint32_t height,
                      uint64_t hwnd)
{
   if (renderer->ops.wsi_init)
      return renderer->ops.wsi_init(renderer, num_images, fds, strides,
                                    virgl_format, width, height, hwnd);
   return false;
}

static inline void
npt_renderer_wsi_present(struct npt_renderer *renderer,
                         uint32_t image_index,
                         int wait_fence_fd)
{
   if (renderer->ops.wsi_present) {
      renderer->ops.wsi_present(renderer, image_index, wait_fence_fd);
      return;
   }
   /* No backend: drop the fence fd so it doesn't leak. */
   if (wait_fence_fd >= 0) {
#ifndef _WIN32
      close(wait_fence_fd);
#endif
   }
}

static inline void
npt_renderer_wsi_destroy(struct npt_renderer *renderer)
{
   if (renderer->ops.wsi_destroy)
      renderer->ops.wsi_destroy(renderer);
}

static inline int
npt_renderer_wsi_drain(struct npt_renderer *renderer,
                       uint32_t timeout_ms,
                       uint32_t out_released_count[NPT_WSI_MAX_IMAGES])
{
   if (out_released_count) {
      for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++)
         out_released_count[i] = 0;
   }
   if (renderer->ops.wsi_drain)
      return renderer->ops.wsi_drain(renderer, timeout_ms, out_released_count);
   return -1;
}

static inline int
npt_renderer_submit_present_fence(struct npt_renderer *renderer,
                                  uint32_t ring_idx)
{
   if (renderer->ops.submit_present_fence)
      return renderer->ops.submit_present_fence(renderer, ring_idx);
   return -1;
}

static inline int
npt_renderer_submit_present_fence_direct(struct npt_renderer *renderer,
                                         uint32_t ring_idx,
                                         void *direct_event, bool *direct_ok)
{
   *direct_ok = false;
   if (renderer->ops.submit_present_fence_direct) {
      return renderer->ops.submit_present_fence_direct(renderer, ring_idx,
                                                       direct_event,
                                                       direct_ok);
   }
   return npt_renderer_submit_present_fence(renderer, ring_idx);
}

static inline uint64_t
npt_renderer_arm_gate_fence(struct npt_renderer *renderer, uint32_t ring_idx)
{
   if (renderer->ops.arm_gate_fence)
      return renderer->ops.arm_gate_fence(renderer, ring_idx);
   return 0;
}

#endif /* NPT_RENDERER_H */
