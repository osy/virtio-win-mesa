/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared machinery of the guest-fabricated swapchains; see
 * npt_swapchain_common.h.
 */

#include "npt_swapchain_common.h"

#if defined(__WINE__)

#include "npt_device.h"
#include "npt_event.h"
#include "npt_renderer.h"
#include "npt_renderer_wine_common.h"
#include "npt_shared_texture.h"
#include "nptunix/npt_unixlib.h"

#include <stdlib.h>
#include <string.h>

#include "neptune-protocol/npt_protocol_client_idxgiswapchain.h"

static_assert(NPT_SC_PRESENT_IMAGES <= NPT_WSI_MAX_IMAGES,
              "present pool exceeds unixlib image table");

static const GUID *const npt_gsc_qi_chain[] = {
   &NPT_IID_IUnknown,
   &NPT_IID_IDXGIObject,
   &NPT_IID_IDXGIDeviceSubObject,
   &NPT_IID_IDXGISwapChain,
   &NPT_IID_IDXGISwapChain1,
   &NPT_IID_IDXGISwapChain2,
   &NPT_IID_IDXGISwapChain3,
   &NPT_IID_IDXGISwapChain4,
   NULL,
};

/* ------------------------------------------------------------------ */
/* held-wrapper helpers                                                */
/* ------------------------------------------------------------------ */

void
npt_gsc_release_held(struct npt_device *dev, void **slot, uint64_t *id)
{
   void *w = *slot;
   uint64_t host_id = *id;
   *slot = NULL;
   *id = 0;
   if (!w)
      return;
   /* The device-level force-destroy drain frees wrappers in arbitrary
    * order; a release on an already-freed wrapper is UAF. */
   if (!dev || !host_id ||
       npt_device_wrapper_cache_is_live(dev, host_id, w))
      npt_com_default_release(w);
}

/* Generic "QI a held wrapper for the caller's riid" helper: the held
 * ref stays ours; QI hands the caller its own. */
HRESULT
npt_gsc_qi_held(void *wrapper, const IID *riid, void **out)
{
   if (!out)
      return NPT_E_POINTER;
   *out = NULL;
   if (!wrapper)
      return NPT_E_FAIL;
   const struct npt_idxgiswapchain_client_vtbl *v =
      (const void *)((struct npt_com_base *)wrapper)->lpVtbl;
   return v->QueryInterface(wrapper, riid, out);
}

/* ------------------------------------------------------------------ */
/* IDLE_NOTIFY drain thread                                            */
/* ------------------------------------------------------------------ */

static DWORD WINAPI
npt_gsc_drain_thread(LPVOID arg)
{
   struct npt_guest_swapchain_common *c = arg;
   struct npt_renderer *renderer = c->com->base.device->renderer;
   while (!atomic_load(&c->drain_exit)) {
      uint32_t released[NPT_WSI_MAX_IMAGES] = { 0 };
      int rc = npt_renderer_wsi_drain(renderer, /*timeout_ms=*/100, released);
      if (rc == -1) {
         Sleep(100);
         continue;
      }
      if (rc < 0)
         break;
      bool any = false;
      for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
         for (uint32_t k = 0; k < released[i]; k++) {
            uint32_t cur = atomic_load(&c->image_inflight[i]);
            /* Releases for a pre-resize generation can outnumber the
             * current inflight count; clamp at zero. */
            while (cur &&
                   !atomic_compare_exchange_weak(&c->image_inflight[i],
                                                 &cur, cur - 1))
               ;
            any = true;
         }
      }
      /* Wake a Present blocked in the reuse gate.  Auto-reset: an
       * unconsumed signal latches, so a release that lands between the
       * gate's inflight check and its wait is not lost. */
      if (any && c->image_release_evt)
         SetEvent(c->image_release_evt);
   }
   return 0;
}

static bool
npt_gsc_drain_start(struct npt_guest_swapchain_common *c)
{
   if (c->drain_started)
      return true;
   atomic_store(&c->drain_exit, 0);
   c->drain_thread = CreateThread(NULL, 0, npt_gsc_drain_thread, c, 0, NULL);
   if (!c->drain_thread) {
      npt_log("%s: drain CreateThread failed err=%lu", c->tag,
              GetLastError());
      return false;
   }
   c->drain_started = true;
   return true;
}

static void
npt_gsc_drain_stop(struct npt_guest_swapchain_common *c, bool shutting_down)
{
   if (!c->drain_started)
      return;
   atomic_store(&c->drain_exit, 1);
   /* At device teardown the drain worker may be inside an X round-trip
    * the display-side close never lets return; skip the join (caller
    * leaks the aux so the worker has valid memory to come back to). */
   if (!shutting_down) {
      WaitForSingleObject(c->drain_thread, INFINITE);
      CloseHandle(c->drain_thread);
      c->drain_thread = NULL;
   }
   c->drain_started = false;
}

/* ------------------------------------------------------------------ */
/* WSI worker thread                                                   */
/* ------------------------------------------------------------------ */

/* Process-unique single-use event tokens.  Bit 63 keeps them disjoint
 * from real Wine HANDLE values in the host's token registry; the one
 * counter serves both swapchain types, so tokens stay unique when a
 * process holds both. */
static _Atomic uint64_t npt_gsc_next_token_low = 1;

uint64_t
npt_gsc_make_token(void)
{
   return npt_com_make_guest_id(NPT_GUEST_KIND_SWAPCHAIN,
                                atomic_fetch_add(&npt_gsc_next_token_low, 1));
}

/* Advance the frame-latency waitable object by one completed frame.
 * Called exactly once per Present (on the worker once the present
 * retires, or synchronously when the frame is dropped) so a waiting
 * app never deadlocks.  Saturates silently at the semaphore's max. */
void
npt_gsc_frame_latency_release(struct npt_guest_swapchain_common *c)
{
   if (c->frame_latency_sem)
      ReleaseSemaphore(c->frame_latency_sem, 1, NULL);
}

static DWORD WINAPI
npt_gsc_wsi_thread(LPVOID arg)
{
   struct npt_guest_swapchain_common *c = arg;
   struct npt_device *dev = c->com->base.device;
   struct npt_renderer *renderer = dev->renderer;
   for (;;) {
      struct npt_gsc_present_entry entry;
      mtx_lock(&c->wsi_mutex);
      while (c->wsi_head == c->wsi_tail &&
             !atomic_load_explicit(&c->wsi_thread_exit, memory_order_acquire))
         cnd_wait(&c->wsi_cond, &c->wsi_mutex);
      if (atomic_load_explicit(&c->wsi_thread_exit, memory_order_acquire)) {
         /* Drop remaining entries without presenting. */
         while (c->wsi_head != c->wsi_tail) {
            struct npt_gsc_present_entry *e =
               &c->wsi_queue[c->wsi_head % NPT_SC_WSI_QUEUE_SIZE];
            if (e->wait_fence_fd >= 0)
               npt_wine_unixlib_close(e->wait_fence_fd);
            if (e->token)
               npt_event_release_token(dev, e->token);
            c->wsi_head++;
            npt_gsc_frame_latency_release(c);
         }
         mtx_unlock(&c->wsi_mutex);
         break;
      }
      entry = c->wsi_queue[c->wsi_head % NPT_SC_WSI_QUEUE_SIZE];
      c->wsi_head++;
      /* Wake a producer waiting on a full queue. */
      cnd_broadcast(&c->wsi_cond);
      mtx_unlock(&c->wsi_mutex);

      /* Owned by entry now; the backend closes/consumes the fd. */
      npt_renderer_wsi_present(renderer, entry.image_index,
                               entry.wait_fence_fd);
      if (entry.token)
         npt_event_release_token(dev, entry.token);
      npt_gsc_frame_latency_release(c);
   }
   return 0;
}

static bool
npt_gsc_wsi_start(struct npt_guest_swapchain_common *c)
{
   if (c->wsi_thread_started)
      return true;
   atomic_store_explicit(&c->wsi_thread_exit, 0, memory_order_release);
   c->wsi_head = 0;
   c->wsi_tail = 0;
   c->wsi_thread = CreateThread(NULL, 0, npt_gsc_wsi_thread, c, 0, NULL);
   if (!c->wsi_thread) {
      npt_log("%s: WSI worker CreateThread failed err=%lu", c->tag,
              GetLastError());
      return false;
   }
   c->wsi_thread_started = true;
   return true;
}

static void
npt_gsc_wsi_stop(struct npt_guest_swapchain_common *c, bool shutting_down)
{
   if (!c->wsi_thread_started)
      return;
   mtx_lock(&c->wsi_mutex);
   atomic_store_explicit(&c->wsi_thread_exit, 1, memory_order_release);
   cnd_broadcast(&c->wsi_cond);
   mtx_unlock(&c->wsi_mutex);
   /* Same skip-the-join hazard as the drain thread: the worker may be
    * wedged inside an X round-trip at device teardown. */
   if (!shutting_down) {
      WaitForSingleObject(c->wsi_thread, INFINITE);
      CloseHandle(c->wsi_thread);
      c->wsi_thread = NULL;
   }
   c->wsi_thread_started = false;
}

void
npt_gsc_wsi_push(struct npt_guest_swapchain_common *c, uint32_t image_index,
                 int wait_fence_fd, uint64_t token)
{
   mtx_lock(&c->wsi_mutex);
   /* The latency ring caps in-flight presents below the queue size;
    * blocking here is defensive (resize transitions). */
   while ((c->wsi_tail - c->wsi_head) >= NPT_SC_WSI_QUEUE_SIZE)
      cnd_wait(&c->wsi_cond, &c->wsi_mutex);
   c->wsi_queue[c->wsi_tail % NPT_SC_WSI_QUEUE_SIZE] =
      (struct npt_gsc_present_entry){
         .image_index = image_index,
         .wait_fence_fd = wait_fence_fd,
         .token = token,
      };
   c->wsi_tail++;
   cnd_signal(&c->wsi_cond);
   mtx_unlock(&c->wsi_mutex);
}

/* ------------------------------------------------------------------ */
/* latency ring                                                        */
/* ------------------------------------------------------------------ */

void
npt_gsc_backpressure_wait(struct npt_guest_swapchain_common *c)
{
   UINT cap = c->max_frame_latency;
   if (cap > NPT_SC_PRESENT_LATENCY)
      cap = NPT_SC_PRESENT_LATENCY;
   if (c->latency_count < cap)
      return;
   int oldest = c->latency_fences[0];
   if (oldest >= 0) {
      if (npt_unixlib_ensure_init() == 0) {
         struct npt_unix_wait_fd_params p = {
            .fd = oldest, .timeout_ms = 1000, .result = -1,
         };
         npt_wine_unix_call(npt_unix_wait_fd, &p);
      }
      npt_wine_unixlib_close(oldest);
   }
   for (uint32_t i = 0; i + 1 < NPT_SC_PRESENT_LATENCY; i++)
      c->latency_fences[i] = c->latency_fences[i + 1];
   c->latency_fences[NPT_SC_PRESENT_LATENCY - 1] = -1;
   c->latency_count--;
}

void
npt_gsc_backpressure_record(struct npt_guest_swapchain_common *c,
                            int fence_fd)
{
   if (fence_fd < 0)
      return;
   if (c->latency_count >= NPT_SC_PRESENT_LATENCY) {
      npt_wine_unixlib_close(fence_fd);
      return;
   }
   c->latency_fences[c->latency_count++] = fence_fd;
}

static void
npt_gsc_backpressure_flush(struct npt_guest_swapchain_common *c)
{
   for (uint32_t i = 0; i < NPT_SC_PRESENT_LATENCY; i++) {
      if (c->latency_fences[i] >= 0) {
         npt_wine_unixlib_close(c->latency_fences[i]);
         c->latency_fences[i] = -1;
      }
   }
   c->latency_count = 0;
}

/* ------------------------------------------------------------------ */
/* present pool + WSI                                                  */
/* ------------------------------------------------------------------ */

void
npt_gsc_release_pool(struct npt_guest_swapchain_common *c)
{
   struct npt_device *dev = c->com->base.device;
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++)
      npt_gsc_release_held(dev, &c->present_tex[i], &c->present_tex_id[i]);
}

/* Stop the workers, drop the latency ring, and destroy the WSI
 * backend; the pool itself is the caller's to release (the D3D12
 * swapchain interleaves its copy-list release).  When the device is
 * shutting down the worker joins are skipped and the caller must leak
 * the aux so the workers keep valid memory to come back to. */
void
npt_gsc_wsi_quiesce(struct npt_guest_swapchain_common *c, bool shutting_down)
{
   npt_gsc_wsi_stop(c, shutting_down);
   npt_gsc_drain_stop(c, shutting_down);
   npt_gsc_backpressure_flush(c);
   if (c->wsi_initialized && !shutting_down)
      npt_renderer_wsi_destroy(c->com->base.device->renderer);
   c->wsi_initialized = false;
   c->wsi_failed = false;
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++)
      atomic_store(&c->image_inflight[i], 0);
}

bool
npt_gsc_wsi_backend_available(struct npt_guest_swapchain_common *c)
{
   struct npt_renderer *renderer = c->com->base.device->renderer;
   if (!renderer->ops.wsi_init || !renderer->ops.create_host_blob) {
      npt_log("%s: transport has no WSI backend; presents will be dropped",
              c->tag);
      c->wsi_failed = true;
      return false;
   }
   return true;
}

/* Bind the (already created) exportable pool to guest blobs, hand the
 * dmabufs to the WSI backend, and start the worker threads.  On any
 * failure the pool is released and presents degrade to the no-WSI
 * path (they silently succeed). */
bool
npt_gsc_wsi_bringup(struct npt_guest_swapchain_common *c, uint32_t fmt)
{
   struct npt_renderer *renderer = c->com->base.device->renderer;

   int fds[NPT_SC_PRESENT_IMAGES];
   uint32_t strides[NPT_SC_PRESENT_IMAGES];
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++)
      fds[i] = -1;

   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      struct npt_shared_texture_desc exp;
      memset(&exp, 0, sizeof(exp));
      if (!npt_shared_texture_export_blob(c->present_tex[i], &exp))
         goto fail;
      if (exp.plane_count != 1 || exp.modifier != 0 /* LINEAR */)
         npt_log("%s: unexpected export (planes=%u modifier=0x%llx)",
                 c->tag, exp.plane_count, (unsigned long long)exp.modifier);
      strides[i] = (uint32_t)exp.planes[0].pitch;

      /* Page-align defensively; the blob create is page-granular. */
      const uint64_t size = (exp.allocation_size + 4095u) & ~4095ull;
      fds[i] = npt_renderer_create_host_blob(renderer, exp.blob_id, size);
      if (fds[i] < 0) {
         npt_log("%s: blob claim failed for image %u", c->tag, i);
         goto fail;
      }
   }

   if (!npt_renderer_wsi_init(renderer, NPT_SC_PRESENT_IMAGES, fds, strides,
                              npt_shared_texture_virgl_format(fmt),
                              c->desc.Width, c->desc.Height,
                              (uint64_t)(uintptr_t)c->hwnd)) {
      npt_log("%s: WSI init failed (hwnd=%p %ux%u)", c->tag,
              (void *)c->hwnd, c->desc.Width, c->desc.Height);
      goto fail;
   }

   /* The unixlib dup'd the fds; ours are done. */
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      npt_wine_unixlib_close(fds[i]);
      fds[i] = -1;
   }

   c->wsi_initialized = true;
   if (!npt_gsc_drain_start(c) || !npt_gsc_wsi_start(c)) {
      /* Without both workers a Present would enqueue frames nothing
       * consumes and eventually block forever on the full queue.  Drop
       * back to the no-WSI path instead. */
      npt_log("%s: present worker startup failed; presents will be dropped",
              c->tag);
      npt_gsc_wsi_quiesce(c, false);
      npt_gsc_release_pool(c);
      c->wsi_failed = true;
      return false;
   }
   return true;

fail:
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      if (fds[i] >= 0)
         npt_wine_unixlib_close(fds[i]);
   }
   npt_gsc_release_pool(c);
   c->wsi_failed = true;
   return false;
}

/* Reuse gate: block (bounded) until the drain thread reports X has
 * released this image, waking on its event rather than polling.  A
 * minimized window never idles; proceed anyway after the cap so the
 * app keeps running. */
void
npt_gsc_reuse_gate(struct npt_guest_swapchain_common *c, uint32_t image)
{
   if (atomic_load(&c->image_inflight[image]) != 0) {
      ULONGLONG deadline = GetTickCount64() + NPT_SC_REUSE_WAIT_MS;
      while (atomic_load(&c->image_inflight[image]) != 0) {
         ULONGLONG now = GetTickCount64();
         if (now >= deadline)
            break;
         DWORD wait_ms = (DWORD)(deadline - now);
         if (c->image_release_evt)
            WaitForSingleObject(c->image_release_evt, wait_ms);
         else
            Sleep(wait_ms < 5 ? wait_ms : 5);
      }
   }
   if (atomic_load(&c->image_inflight[image]) != 0) {
      /* The image is still on-screen after the wait: the compositor
       * isn't consuming frames (hidden/minimized).  After a few such
       * presents report OCCLUDED so the app backs off; still present
       * (some apps only re-render on OCCLUDED clear). */
      if (c->occluded_streak < NPT_SC_OCCLUDED_STREAK)
         c->occluded_streak++;
   } else {
      c->occluded_streak = 0;
   }
}

void
npt_gsc_query_window_size(struct npt_guest_swapchain_common *c,
                          UINT *width, UINT *height)
{
   RECT rect;
   if (c->hwnd && GetClientRect(c->hwnd, &rect)) {
      *width = (UINT)(rect.right - rect.left);
      *height = (UINT)(rect.bottom - rect.top);
   }
   if (!*width)
      *width = 1;
   if (!*height)
      *height = 1;
}

/* Resize a windowed swap chain's client area to w x h. */
void
npt_gsc_window_resize(HWND hwnd, uint32_t w, uint32_t h)
{
   RECT newRect = { 0, 0, (LONG)w, (LONG)h };
   RECT oldRect = { 0, 0, 0, 0 };
   GetWindowRect(hwnd, &oldRect);
   AdjustWindowRectEx(&newRect, GetWindowLongW(hwnd, GWL_STYLE), FALSE,
                      GetWindowLongW(hwnd, GWL_EXSTYLE));
   SetRect(&newRect, 0, 0, newRect.right - newRect.left,
           newRect.bottom - newRect.top);
   OffsetRect(&newRect, oldRect.left, oldRect.top);
   MoveWindow(hwnd, newRect.left, newRect.top,
              newRect.right - newRect.left, newRect.bottom - newRect.top,
              TRUE);
}

/* ------------------------------------------------------------------ */
/* outputs                                                             */
/* ------------------------------------------------------------------ */

void
npt_gsc_assign_output(struct npt_guest_swapchain_common *c,
                      IDXGIOutput **slot, IDXGIOutput *new_val)
{
   IDXGIOutput *old;
   mtx_lock(&c->output_lock);
   old = *slot;
   if (new_val)
      npt_gsc_output_vtbl(new_val)->AddRef(new_val);
   *slot = new_val;
   mtx_unlock(&c->output_lock);
   if (old)
      npt_gsc_output_vtbl(old)->Release(old);
}

IDXGIOutput *
npt_gsc_load_output_addref(struct npt_guest_swapchain_common *c,
                           IDXGIOutput **slot)
{
   IDXGIOutput *v;
   mtx_lock(&c->output_lock);
   v = *slot;
   if (v)
      npt_gsc_output_vtbl(v)->AddRef(v);
   mtx_unlock(&c->output_lock);
   return v;
}

void
npt_gsc_release_outputs(struct npt_guest_swapchain_common *c)
{
   if (c->fullscreen_target)
      npt_gsc_output_vtbl(c->fullscreen_target)->Release(c->fullscreen_target);
   if (c->containing_output)
      npt_gsc_output_vtbl(c->containing_output)->Release(c->containing_output);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

void
npt_gsc_init(struct npt_guest_swapchain_common *c, struct npt_com_base *com,
             const DXGI_SWAP_CHAIN_DESC1 *desc1,
             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
             HWND hwnd, const char *tag)
{
   c->com = com;
   c->tag = tag;
   mtx_init(&c->present_mutex, mtx_plain);
   mtx_init(&c->output_lock, mtx_plain);
   mtx_init(&c->wsi_mutex, mtx_plain);
   cnd_init(&c->wsi_cond);
   /* Auto-reset: an unconsumed signal latches, so a release that races
    * the reuse gate's inflight check is not lost. */
   c->image_release_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
   for (uint32_t i = 0; i < NPT_SC_PRESENT_LATENCY; i++)
      c->latency_fences[i] = -1;

   c->desc = *desc1;
   if (fs_desc) {
      c->fs_desc = *fs_desc;
   } else {
      memset(&c->fs_desc, 0, sizeof(c->fs_desc));
      c->fs_desc.Windowed = 1;
   }
   c->hwnd = hwnd;
   /* Born windowed; a fullscreen creation desc is realized by the
    * factory's post-creation npt_swapchain_apply_initial_fullscreen ->
    * SetFullscreenState. */
   c->fullscreen = 0;
   /* Waitable swapchains default to 1 frame of latency (DXGI); others
    * to 3.  The waitable object is a semaphore released once per
    * frame. */
   const bool waitable =
      (c->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
   c->max_frame_latency = waitable ? 1 : 3;
   if (waitable) {
      c->frame_latency_sem = CreateSemaphoreW(
         NULL, (LONG)c->max_frame_latency,
         (LONG)NPT_SC_MAX_SWAP_CHAIN_BUFFERS, NULL);
      if (!c->frame_latency_sem)
         npt_log("%s: frame-latency semaphore create failed err=%lu; "
                 "waitable object unavailable", c->tag, GetLastError());
   }
   c->background.a = 1.0f;

   /* Normalize: DXGI fills zero dimensions from the window and
    * defaults the format. */
   if (!c->desc.Width || !c->desc.Height) {
      UINT w = c->desc.Width, h = c->desc.Height;
      npt_gsc_query_window_size(c, &w, &h);
      c->desc.Width = w;
      c->desc.Height = h;
   }
   if (!c->desc.Format)
      c->desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   if (!c->desc.SampleDesc.Count)
      c->desc.SampleDesc.Count = 1;
}

void
npt_gsc_fini(struct npt_guest_swapchain_common *c)
{
   cnd_destroy(&c->wsi_cond);
   mtx_destroy(&c->wsi_mutex);
   mtx_destroy(&c->present_mutex);
   mtx_destroy(&c->output_lock);
   /* Workers joined by the WSI teardown, so these are unreferenced. */
   if (c->image_release_evt)
      CloseHandle(c->image_release_evt);
   if (c->frame_latency_sem)
      CloseHandle(c->frame_latency_sem);
}

/* ------------------------------------------------------------------ */
/* IUnknown / IDXGIObject                                              */
/* ------------------------------------------------------------------ */

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_QueryInterface(void *self, REFIID riid, void **ppvObject)
{
   HRESULT hr = npt_com_default_query_interface_chain(
      self, riid, ppvObject, npt_gsc_qi_chain);
   return hr == NPT_S_OK ? hr : NPT_E_NOINTERFACE;
}

ULONG NPT_STDMETHODCALLTYPE
npt_gsc_AddRef(void *self)
{
   return npt_com_default_addref(self);
}

ULONG NPT_STDMETHODCALLTYPE
npt_gsc_Release(void *self)
{
   return npt_com_default_release(self);
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetPrivateData(void *self, const GUID *Name, UINT DataSize,
                       const void *pData)
{
   (void)self; (void)Name; (void)DataSize; (void)pData;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetPrivateDataInterface(void *self, const GUID *Name,
                                const IUnknown *pUnknown)
{
   (void)self; (void)Name; (void)pUnknown;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetPrivateData(void *self, const GUID *Name, UINT *pDataSize,
                       void *pData)
{
   (void)self; (void)Name; (void)pData;
   if (pDataSize)
      *pDataSize = 0;
   return NPT_DXGI_ERROR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* desc / window                                                       */
/* ------------------------------------------------------------------ */

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetDesc(void *self, DXGI_SWAP_CHAIN_DESC *pDesc)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pDesc)
      return NPT_E_POINTER;
   memset(pDesc, 0, sizeof(*pDesc));
   pDesc->BufferDesc.Width = c->desc.Width;
   pDesc->BufferDesc.Height = c->desc.Height;
   pDesc->BufferDesc.RefreshRate = c->fs_desc.RefreshRate;
   pDesc->BufferDesc.Format = c->desc.Format;
   pDesc->BufferDesc.ScanlineOrdering = c->fs_desc.ScanlineOrdering;
   pDesc->BufferDesc.Scaling = c->fs_desc.Scaling;
   pDesc->SampleDesc = c->desc.SampleDesc;
   pDesc->BufferUsage = c->desc.BufferUsage;
   pDesc->BufferCount = c->desc.BufferCount;
   pDesc->OutputWindow = c->hwnd;
   pDesc->Windowed = c->fs_desc.Windowed;
   pDesc->SwapEffect = c->desc.SwapEffect;
   pDesc->Flags = c->desc.Flags;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetDesc1(void *self, DXGI_SWAP_CHAIN_DESC1 *pDesc)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pDesc)
      return NPT_E_POINTER;
   *pDesc = c->desc;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFullscreenDesc(void *self, DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pDesc)
      return NPT_E_POINTER;
   *pDesc = c->fs_desc;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetHwnd(void *self, HWND *pHwnd)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pHwnd)
      return NPT_E_POINTER;
   *pHwnd = c->hwnd;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetCoreWindow(void *self, const IID *refiid, void **ppUnk)
{
   (void)self; (void)refiid;
   if (ppUnk)
      *ppUnk = NULL;
   return NPT_DXGI_ERROR_INVALID_CALL;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFullscreenState(void *self, BOOL *pFullscreen,
                           IDXGIOutput **ppTarget)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (pFullscreen)
      *pFullscreen = c->fullscreen;
   if (ppTarget)
      *ppTarget = c->fullscreen
                     ? npt_gsc_load_output_addref(c, &c->fullscreen_target)
                     : NULL;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetRestrictToOutput(void *self, IDXGIOutput **ppRestrictToOutput)
{
   (void)self;
   if (!ppRestrictToOutput)
      return NPT_E_POINTER;
   *ppRestrictToOutput = NULL;
   return NPT_S_OK;
}

/* ------------------------------------------------------------------ */
/* stats / misc state                                                  */
/* ------------------------------------------------------------------ */

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFrameStatistics(void *self, DXGI_FRAME_STATISTICS *pStats)
{
   (void)self;
   if (!pStats)
      return NPT_E_POINTER;
   memset(pStats, 0, sizeof(*pStats));
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetLastPresentCount(void *self, UINT *pLastPresentCount)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pLastPresentCount)
      return NPT_E_POINTER;
   *pLastPresentCount = c->present_count;
   return NPT_S_OK;
}

BOOL NPT_STDMETHODCALLTYPE
npt_gsc_IsTemporaryMonoSupported(void *self)
{
   (void)self;
   return 0;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetBackgroundColor(void *self, const DXGI_RGBA *pColor)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pColor)
      return NPT_E_POINTER;
   c->background = *pColor;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetBackgroundColor(void *self, DXGI_RGBA *pColor)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pColor)
      return NPT_E_POINTER;
   *pColor = c->background;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetRotation(void *self, DXGI_MODE_ROTATION Rotation)
{
   (void)self;
   return Rotation == DXGI_MODE_ROTATION_IDENTITY
             ? NPT_S_OK : NPT_DXGI_ERROR_INVALID_CALL;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetRotation(void *self, DXGI_MODE_ROTATION *pRotation)
{
   (void)self;
   if (!pRotation)
      return NPT_E_POINTER;
   *pRotation = DXGI_MODE_ROTATION_IDENTITY;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetSourceSize(void *self, UINT Width, UINT Height)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!Width || !Height ||
       Width > c->desc.Width || Height > c->desc.Height)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if (Width != c->desc.Width || Height != c->desc.Height)
      npt_log("%s: SetSourceSize %ux%u ignored (full %ux%u)", c->tag,
              Width, Height, c->desc.Width, c->desc.Height);
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetSourceSize(void *self, UINT *pWidth, UINT *pHeight)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pWidth || !pHeight)
      return NPT_E_POINTER;
   *pWidth = c->desc.Width;
   *pHeight = c->desc.Height;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetMaximumFrameLatency(void *self, UINT MaxLatency)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!MaxLatency || MaxLatency > NPT_SC_MAX_SWAP_CHAIN_BUFFERS)
      return NPT_DXGI_ERROR_INVALID_CALL;
   /* Waitable object: grow the semaphore by the delta and never shrink
    * it (matching DXGI -- shrinking could strand an app mid-wait). */
   if (c->frame_latency_sem && MaxLatency > c->max_frame_latency)
      ReleaseSemaphore(c->frame_latency_sem,
                       (LONG)(MaxLatency - c->max_frame_latency), NULL);
   c->max_frame_latency = MaxLatency;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetMaximumFrameLatency(void *self, UINT *pMaxLatency)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!pMaxLatency)
      return NPT_E_POINTER;
   *pMaxLatency = c->max_frame_latency;
   return NPT_S_OK;
}

HANDLE NPT_STDMETHODCALLTYPE
npt_gsc_GetFrameLatencyWaitableObject(void *self)
{
   struct npt_guest_swapchain_common *c = npt_gsc(self);
   if (!c->frame_latency_sem)
      return NULL;
   /* Hand the app its own handle (DXGI semantics); the swapchain keeps
    * the original and closes it at teardown. */
   HANDLE dup = NULL;
   DuplicateHandle(GetCurrentProcess(), c->frame_latency_sem,
                   GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
   return dup;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetMatrixTransform(void *self, const DXGI_MATRIX_3X2_F *pMatrix)
{
   (void)self; (void)pMatrix;
   return NPT_DXGI_ERROR_INVALID_CALL;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetMatrixTransform(void *self, DXGI_MATRIX_3X2_F *pMatrix)
{
   (void)self;
   if (!pMatrix)
      return NPT_E_POINTER;
   memset(pMatrix, 0, sizeof(*pMatrix));
   pMatrix->_11 = 1.0f;
   pMatrix->_22 = 1.0f;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_CheckColorSpaceSupport(void *self, DXGI_COLOR_SPACE_TYPE ColorSpace,
                               UINT *pColorSpaceSupport)
{
   (void)self;
   if (!pColorSpaceSupport)
      return NPT_E_POINTER;
   *pColorSpaceSupport =
      ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
         ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT : 0;
   return NPT_S_OK;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetColorSpace1(void *self, DXGI_COLOR_SPACE_TYPE ColorSpace)
{
   (void)self;
   return ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
             ? NPT_S_OK : NPT_DXGI_ERROR_INVALID_CALL;
}

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetHDRMetaData(void *self, DXGI_HDR_METADATA_TYPE Type, UINT Size,
                       void *pMetaData)
{
   (void)self; (void)Type; (void)Size; (void)pMetaData;
   return NPT_S_OK;
}

#endif /* __WINE__ */
