/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGISwapChain{,1..4}: Present drives the dmabuf WSI flip and
 * spawns the release-forwarder thread; GetBuffer caches per-(idx,
 * iid) accessors; ResizeBuffers* drop that cache before forwarding
 * (host retires the old backbuffer set).
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_env.h"
#include "npt_object.h"
#include "npt_overrides.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "npt_renderer_wine_common.h"
#include "npt_ring.h"
#include "npt_transport_defs.h"
#include "nptunix/npt_unixlib.h"

#include <stdatomic.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "neptune-protocol/npt_protocol_client_idxgiadapter.h"
#include "neptune-protocol/npt_protocol_client_idxgidevice.h"
#include "neptune-protocol/npt_protocol_client_idxgidevicesubobject.h"
#include "neptune-protocol/npt_protocol_client_idxgioutput.h"
#include "neptune-protocol/npt_protocol_client_idxgiswapchain.h"
#include "neptune-protocol/npt_protocol_guest_idxgiswapchain.h"

static_assert(NPT_SWAPCHAIN_MAX_IMAGES == NPT_WSI_MAX_IMAGES,
              "NPT_SWAPCHAIN_MAX_IMAGES and NPT_WSI_MAX_IMAGES "
              "must match");

struct npt_swapchain_buffer_cache_entry {
   GUID iid;
   _Atomic(void *) wrapper;
   /* Pinned host_id of the cached wrapper; used by sc_drop_buffer_cache
    * to verify the wrapper is still alive in the device's cache before
    * releasing.  Without this check, a teardown that destroys backbuffer
    * wrappers via npt_device_destroy's drain (force_destroy bypasses
    * release cascade) leaves dangling pointers here, and the subsequent
    * swapchain teardown's release-on-dangling-ptr is UAF. */
   _Atomic uint64_t host_id;
};

#define NPT_SWAPCHAIN_BUFFER_CACHE_MAX NPT_SWAPCHAIN_MAX_IMAGES

/* Backpressure: ring of dup'd present_fence FDs (sync_files signaling
 * when the host GPU finished each frame).  At Present(), if the ring
 * is full we wait on the oldest before submitting, capping app
 * runahead at NPT_PRESENT_LATENCY frames.  Mirrors DXGI's
 * MaximumFrameLatency semantics (default 3). */
#define NPT_PRESENT_LATENCY 3

/* Async WSI Present worker.  poll(wait_fence_fd) + xcb_present_pixmap
 * run on a per-swapchain worker thread so the d3d11 thread returns
 * from Present without blocking on the host fence; backpressure on
 * N-3 still paces the app via sc_present_backpressure_wait. */
#define NPT_WSI_PRESENT_QUEUE_SIZE 8  /* > NPT_PRESENT_LATENCY */

struct npt_wsi_present_entry {
   uint32_t image_index;
   int      wait_fence_fd;  /* -1 = no fence; ownership transferred
                             * to the worker on push. */
};

/* Per-swapchain storage: WSI init state, GetBuffer wrapper cache, and
 * the X11 release-forwarder thread that ships IMAGE_RELEASE back to
 * the host's dmabuf mailbox. */
struct npt_dxgi_swapchain_aux {
   struct npt_com_base *com;

   bool wsi_initialized;
   /* Number of dmabuf images the host published in
    * npt_swapchain_images_info, NOT the DXGI app-visible BufferCount.
    * Bounds the WSI mailbox / forwarder iteration; the app-visible
    * back-buffer count rides on the host's IDXGISwapChain wrapper. */
   uint32_t wsi_image_count;
   uint32_t image_index;

   uint32_t host_ring_idx;

   struct npt_swapchain_buffer_cache_entry
      buffer_cache[NPT_SWAPCHAIN_BUFFER_CACHE_MAX];

#if defined(_WIN32)
   HANDLE forwarder_thread;
#else
   thrd_t forwarder_thread;
#endif
   bool forwarder_started;
   _Atomic int forwarder_exit;

   int latency_fences[NPT_PRESENT_LATENCY];
   uint32_t latency_count;

   /* Async WSI Present worker.  See NPT_WSI_PRESENT_QUEUE_SIZE for
    * the ring size.  head/tail wrap modulo the size; the gap
    * (tail - head) is the number of pending entries. */
   mtx_t       wsi_mutex;
   cnd_t       wsi_cond;
   struct npt_wsi_present_entry wsi_queue[NPT_WSI_PRESENT_QUEUE_SIZE];
   uint32_t    wsi_head;
   uint32_t    wsi_tail;
   _Atomic int wsi_thread_exit;
   bool        wsi_thread_started;
#if defined(_WIN32)
   HANDLE      wsi_thread;
#else
   thrd_t      wsi_thread;
#endif

   /* IDXGIOutput-related state (see comment divider below). */
   mtx_t       output_lock;
   IDXGIOutput *fullscreen_target;
   IDXGIOutput *containing_output;
};

static void npt_dxgi_swapchain_aux_init(struct npt_com_base *com,
                                        struct npt_device *dev,
                                        uint64_t host_id);
static void npt_dxgi_swapchain_aux_destroy(void *aux);

#define NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN4(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiswapchain4, m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN3(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiswapchain3, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN4(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN2(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiswapchain2, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN3(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN1(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiswapchain1, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN2(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiswapchain, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN1(m, f)

static const GUID *const swapchain_tiers[] = {
   &NPT_IID_IDXGISwapChain,  &NPT_IID_IDXGISwapChain1,
   &NPT_IID_IDXGISwapChain2, &NPT_IID_IDXGISwapChain3,
   &NPT_IID_IDXGISwapChain4, NULL,
};

static inline struct npt_dxgi_swapchain_aux *
sc_aux(void *self)
{
   return ((struct npt_com_base *)self)->aux;
}

static void sc_forwarder_start(struct npt_dxgi_swapchain_aux *s);
static void sc_forwarder_stop(struct npt_dxgi_swapchain_aux *s);
static void sc_wsi_start(struct npt_dxgi_swapchain_aux *s);
static void sc_wsi_stop(struct npt_dxgi_swapchain_aux *s);
static void sc_wsi_push(struct npt_dxgi_swapchain_aux *s,
                        uint32_t image_index, int wait_fence_fd);

/* The host D3D library defers Vulkan surface creation until the
 * first Present, so dmabuf FDs only exist after that. */
static void
sc_present_init_wsi(struct npt_dxgi_swapchain_aux *s)
{
   struct npt_device *dev = s->com->base.device;
   struct npt_renderer *renderer = dev->renderer;

   size_t info_off = 0;
   struct npt_renderer_shmem *info_shmem =
      npt_device_alloc_data(dev, sizeof(struct npt_swapchain_images_info),
                            &info_off);
   if (!info_shmem) {
      npt_log("sc_present_init_wsi: cannot allocate swapchain images info shmem");
      return;
   }
   memset((uint8_t *)info_shmem->mmap_ptr + info_off, 0,
          sizeof(struct npt_swapchain_images_info));

   /* Same dispatch thread as Present/GetBuffer/ResizeBuffers.  data_off
    * is required because the info shmem comes from a bump-allocated
    * pool — the host has to deposit the populated struct at the
    * caller's offset, not the start of the resource. */
   npt_dispatch_wsi_get_swapchain_images(npt_com_self_ring(s->com),
                                         s->com->base.id,
                                         info_shmem->res_id,
                                         (uint32_t)info_off);

   const struct npt_swapchain_images_info *info =
      (const struct npt_swapchain_images_info *)
         ((uint8_t *)info_shmem->mmap_ptr + info_off);

   if (!info->num_images || !info->width || !info->height) {
      npt_log("sc_present_init_wsi: swapchain images info empty (num=%u %ux%u)",
              info->num_images, info->width, info->height);
      npt_renderer_shmem_unref(renderer, info_shmem);
      return;
   }

   int fds[NPT_SWAPCHAIN_MAX_IMAGES];
   uint32_t strides[NPT_SWAPCHAIN_MAX_IMAGES];
   uint32_t n = info->num_images;
   if (n > NPT_SWAPCHAIN_MAX_IMAGES) n = NPT_SWAPCHAIN_MAX_IMAGES;
   for (uint32_t i = 0; i < n; i++) {
      strides[i] = info->images[i].stride;
      /* Stride accounts for format bpp + row padding. */
      const uint64_t blob_size = (uint64_t)strides[i] * info->height;
      fds[i] = npt_renderer_create_host_blob(renderer,
                                             info->images[i].blob_id,
                                             blob_size);
   }

   /* OutputWindow lets the Wine WSI backend resolve the underlying X11
    * window via winex11.drv's __wine_x11_whole_window property.  Cached
    * by the host swapchain wrapper, so this is a single round-trip. */
   DXGI_SWAP_CHAIN_DESC desc;
   memset(&desc, 0, sizeof(desc));
   npt_idxgiswapchain_default_GetDesc(s->com, &desc);

   if (npt_renderer_wsi_init(renderer, n, fds, strides,
                             info->virgl_format,
                             info->width, info->height,
                             (uint64_t)(uintptr_t)desc.OutputWindow)) {
      s->wsi_initialized = true;
      s->wsi_image_count = n;
      sc_forwarder_start(s);
      sc_wsi_start(s);
   } else {
      npt_log("sc_present_init_wsi: WSI init failed");
   }

   npt_renderer_shmem_unref(renderer, info_shmem);
}

static void
sc_send_image_release(struct npt_dxgi_swapchain_aux *s, uint32_t image_index)
{
   npt_dispatch_wsi_image_release(s->com->base.device->renderer,
                                  s->com->base.id, image_index);
}

#if defined(_WIN32)
static DWORD WINAPI
sc_forwarder_thread(LPVOID arg)
#else
static int
sc_forwarder_thread(void *arg)
#endif
{
   struct npt_dxgi_swapchain_aux *s = arg;
   struct npt_renderer *renderer = s->com->base.device->renderer;
   while (!atomic_load(&s->forwarder_exit)) {
      uint32_t released_count[NPT_WSI_MAX_IMAGES] = { 0 };
      int rc = npt_renderer_wsi_drain(renderer, /*timeout_ms=*/100,
                                      released_count);
      if (rc == -1) {
#if defined(_WIN32)
         Sleep(100);
#else
         struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
         nanosleep(&ts, NULL);
#endif
         continue;
      }
      if (rc < 0)
         break;
      for (uint32_t idx = 0; idx < s->wsi_image_count && idx < NPT_WSI_MAX_IMAGES; idx++) {
         for (uint32_t k = 0; k < released_count[idx]; k++)
            sc_send_image_release(s, idx);
      }
   }
   return 0;
}

static void
sc_forwarder_start(struct npt_dxgi_swapchain_aux *s)
{
   if (s->forwarder_started)
      return;
   atomic_store(&s->forwarder_exit, 0);
#if defined(_WIN32)
   s->forwarder_thread = CreateThread(NULL, 0, sc_forwarder_thread, s, 0, NULL);
   if (!s->forwarder_thread) {
      npt_log("forwarder: CreateThread failed err=%lu", GetLastError());
      return;
   }
#else
   if (thrd_create(&s->forwarder_thread, sc_forwarder_thread, s) != thrd_success)
      return;
#endif
   s->forwarder_started = true;
}

static void
sc_forwarder_stop(struct npt_dxgi_swapchain_aux *s)
{
   if (!s->forwarder_started)
      return;
   atomic_store(&s->forwarder_exit, 1);

#if defined(_WIN32)
   WaitForSingleObject(s->forwarder_thread, INFINITE);
   CloseHandle(s->forwarder_thread);
   s->forwarder_thread = NULL;
#else
   thrd_join(s->forwarder_thread, NULL);
#endif
   s->forwarder_started = false;
}

/* ---------------------------------------------------------------------------
 * Async WSI Present worker
 *
 *  Producer (d3d11/Present thread):  sc_wsi_push enqueues (image_index,
 *      wait_fence_fd) and returns.  Ownership of wait_fence_fd
 *      transfers to the queue.
 *  Consumer (sc_wsi_thread):  pops in FIFO order, calls
 *      npt_renderer_wsi_present which routes to the unixlib's
 *      npt_do_wsi_present (poll + xcb_present_pixmap).
 *  Order guarantee: single producer + single consumer + FIFO queue
 *      preserves Present order to X.
 *  Capacity: NPT_WSI_PRESENT_QUEUE_SIZE entries (> NPT_PRESENT_LATENCY)
 *      so backpressure_wait is the bound that's hit first; if the
 *      queue ever fills, push waits on wsi_cond rather than spinning.
 * ------------------------------------------------------------------------- */

#if defined(_WIN32)
static DWORD WINAPI
sc_wsi_thread(LPVOID arg)
#else
static int
sc_wsi_thread(void *arg)
#endif
{
   struct npt_dxgi_swapchain_aux *s = arg;
   struct npt_renderer *renderer = s->com->base.device->renderer;
   for (;;) {
      struct npt_wsi_present_entry entry;
      mtx_lock(&s->wsi_mutex);
      while (s->wsi_head == s->wsi_tail
          && !atomic_load_explicit(&s->wsi_thread_exit, memory_order_acquire))
         cnd_wait(&s->wsi_cond, &s->wsi_mutex);
      /* Drain remaining entries via the process-global unixlib, which
       * outlives the renderer.  Keeps sc_wsi_stop's skip path safe. */
      if (atomic_load_explicit(&s->wsi_thread_exit, memory_order_acquire)) {
         while (s->wsi_head != s->wsi_tail) {
            int fd = s->wsi_queue[s->wsi_head %
                                  NPT_WSI_PRESENT_QUEUE_SIZE].wait_fence_fd;
            if (fd >= 0)
               npt_wine_unixlib_close(fd);
            s->wsi_head++;
         }
         mtx_unlock(&s->wsi_mutex);
         break;
      }
      entry = s->wsi_queue[s->wsi_head % NPT_WSI_PRESENT_QUEUE_SIZE];
      s->wsi_head++;
      /* Wake any producer waiting on a full queue. */
      cnd_broadcast(&s->wsi_cond);
      mtx_unlock(&s->wsi_mutex);

      /* Owned by entry now; npt_renderer_wsi_present closes it. */
      npt_renderer_wsi_present(renderer, entry.image_index,
                               entry.wait_fence_fd);
   }
   return 0;
}

static void
sc_wsi_start(struct npt_dxgi_swapchain_aux *s)
{
   if (s->wsi_thread_started)
      return;
   atomic_store_explicit(&s->wsi_thread_exit, 0, memory_order_release);
   s->wsi_head = 0;
   s->wsi_tail = 0;
#if defined(_WIN32)
   s->wsi_thread = CreateThread(NULL, 0, sc_wsi_thread, s, 0, NULL);
   if (!s->wsi_thread) {
      npt_log("wsi worker: CreateThread failed err=%lu", GetLastError());
      return;
   }
#else
   if (thrd_create(&s->wsi_thread, sc_wsi_thread, s) != thrd_success)
      return;
#endif
   s->wsi_thread_started = true;
}

static void
sc_wsi_stop(struct npt_dxgi_swapchain_aux *s)
{
   if (!s->wsi_thread_started)
      return;
   mtx_lock(&s->wsi_mutex);
   atomic_store_explicit(&s->wsi_thread_exit, 1, memory_order_release);
   cnd_broadcast(&s->wsi_cond);
   mtx_unlock(&s->wsi_mutex);

   /* At device teardown, the worker may be inside an X11 round-trip
    * that the server-side close never lets return.  Skip the join;
    * the aux is leaked so the still-running worker has valid memory
    * to come back to (see npt_dxgi_swapchain_aux_destroy). */
   if (s->com && s->com->base.device &&
       npt_device_is_shutting_down(s->com->base.device)) {
      s->wsi_thread_started = false;
      return;
   }
#if defined(_WIN32)
   WaitForSingleObject(s->wsi_thread, INFINITE);
   CloseHandle(s->wsi_thread);
   s->wsi_thread = NULL;
#else
   thrd_join(s->wsi_thread, NULL);
#endif
   s->wsi_thread_started = false;
}

static void
sc_wsi_push(struct npt_dxgi_swapchain_aux *s,
            uint32_t image_index, int wait_fence_fd)
{
   mtx_lock(&s->wsi_mutex);
   /* Block if the queue is full.  NPT_PRESENT_LATENCY (=3) caps
    * in-flight Presents so this normally doesn't trigger; defensive
    * for resize transitions that drain in-flight work. */
   while ((s->wsi_tail - s->wsi_head) >= NPT_WSI_PRESENT_QUEUE_SIZE)
      cnd_wait(&s->wsi_cond, &s->wsi_mutex);
   s->wsi_queue[s->wsi_tail % NPT_WSI_PRESENT_QUEUE_SIZE] =
      (struct npt_wsi_present_entry){
         .image_index   = image_index,
         .wait_fence_fd = wait_fence_fd,
      };
   s->wsi_tail++;
   cnd_signal(&s->wsi_cond);
   mtx_unlock(&s->wsi_mutex);
}

/* Backpressure: wait on the oldest in-flight present_fence so the app
 * can't run more than NPT_PRESENT_LATENCY frames ahead of the host GPU.
 * Mirrors DXGI's MaximumFrameLatency semantics (default 3).  Routes
 * the wait through the unixlib bridge because the PE side has no
 * poll(2). */
static void
sc_present_backpressure_wait(struct npt_dxgi_swapchain_aux *s)
{
   if (s->latency_count < NPT_PRESENT_LATENCY)
      return;
   int oldest = s->latency_fences[0];
   if (oldest >= 0) {
      if (npt_unixlib_ensure_init() == 0) {
         struct npt_unix_wait_fd_params p = {
            .fd = oldest, .timeout_ms = 1000, .result = -1,
         };
         npt_wine_unix_call(npt_unix_wait_fd, &p);
      }
      npt_wine_unixlib_close(oldest);
   }
   for (uint32_t i = 0; i < NPT_PRESENT_LATENCY - 1; i++)
      s->latency_fences[i] = s->latency_fences[i + 1];
   s->latency_fences[NPT_PRESENT_LATENCY - 1] = -1;
   s->latency_count--;
}

static void
sc_present_backpressure_record(struct npt_dxgi_swapchain_aux *s, int fence_fd)
{
   if (fence_fd < 0 || s->latency_count >= NPT_PRESENT_LATENCY)
      return;
   s->latency_fences[s->latency_count++] = fence_fd;
}

/* First Present: fetch dmabuf info + start release forwarder.
 * Subsequent Presents: flip via DRI3+Present. */
static HRESULT NPT_STDMETHODCALLTYPE
sc_Present_override(void *self, UINT SyncInterval, UINT Flags)
{
   struct npt_dxgi_swapchain_aux *s = sc_aux(self);
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);
   struct npt_renderer *renderer = dev->renderer;

   uint64_t g_t0 = NPT_DEBUG(PRESENT_TIMING) ? npt_profile_now_ns() : 0;

   /* CPU-runahead backpressure: blocks the caller until the host GPU
    * has progressed.  Mutter + Xwayland defeats msc-based DRI3 vsync
    * (PRESENT_ASYNC is used for compositor-friendly behavior), so the
    * host-vblank pacing is replaced with a fence-based queue limit.
    * Combined with the SyncInterval-derived time pace below this
    * matches DXGI's two back-pressure axes
    * (MaximumFrameLatency + vsync). */
   sc_present_backpressure_wait(s);
   uint64_t g_t1 = NPT_DEBUG(PRESENT_TIMING) ? npt_profile_now_ns() : 0;

   /* SyncInterval pacing model:
    *
    * - SyncInterval == 0 (no vsync): fire-and-forget Present.  Host
    *   runs the per-Present submit async; the guest doesn't wait, only
    *   NPT_PRESENT_LATENCY=3 + host's MaxFrameLatency bound runahead.
    *
    * - SyncInterval > 0: synchronous Present with the user's value
    *   passed through to the host.  The host's present path adds a
    *   vkWaitForFences on the per-Present empty-submit fence, blocking
    *   its submission-queue thread until the GPU has finished frame N.
    *   The host's dispatch thread (== our npt-ring-1, because the
    *   npt_call hasn't returned yet) blocks on the next acquireNextImage
    *   too, holding the guest npt_call_IDXGISwapChain_Present open
    *   until GPU completion -- DXGI's SyncInterval backpressure paced
    *   to GPU completion rather than CPU-timer vblank estimation. */
   HRESULT hr;
   if (SyncInterval == 0) {
      npt_async_IDXGISwapChain_Present(
         npt_com_self_ring(self), self_id, SyncInterval, Flags);
      hr = NPT_S_OK;
   } else {
      hr = npt_call_IDXGISwapChain_Present(
         npt_com_self_ring(self), self_id, SyncInterval, Flags);
   }
   uint64_t g_t2 = NPT_DEBUG(PRESENT_TIMING) ? npt_profile_now_ns() : 0;

   /* Stutter detector: emits a STUTTER log when the Present-to-Present
    * gap exceeds NPT_STUTTER_MS, with per-frame deltas + top cmd_types. */
   npt_profile_present_marker();

   if (!s->wsi_initialized)
      sc_present_init_wsi(s);

   if (s->wsi_initialized && s->wsi_image_count) {
      uint32_t flip_idx = s->image_index % s->wsi_image_count;

      /* Sync_file backed by the host's per-Present GPU-done fence,
       * exposed via FENCE_FD_OUT.  The unixlib uses it as
       * xcb_present_pixmap's wait_fence on DRI3 1.4, or polls before
       * scheduling on older fallbacks; cross-VM dmabuf has no implicit
       * reservation sync, so without this the X server can read while
       * the host renderer is still writing. */
      int wait_fence_fd =
         npt_renderer_submit_present_fence(renderer, s->host_ring_idx);

      /* Record a duped FD for backpressure before WSI consumes the
       * original.  CRITICAL: do NOT call submit_present_fence twice
       * per frame — the host's present_done_fifo is pushed once per
       * actual present (onPresentSubmitted), and a second pop steals
       * the next frame's slot, deadlocking Present once the ring
       * fills.  Next Present blocks on this dup'd FD when the ring
       * is full. */
      int backpressure_fence_fd = npt_wine_unixlib_dup(wait_fence_fd);
      sc_present_backpressure_record(s, backpressure_fence_fd);

      if (NPT_DEBUG(PRESENT_ORDER))
         npt_profile_log_present_order(flip_idx, s->image_index,
                                       s->wsi_image_count, wait_fence_fd);

      /* Hand the (image_index, wait_fence_fd) pair to the worker
       * thread.  poll(wait_fence_fd) + xcb_present_pixmap happen
       * there; this thread returns from Present without blocking on
       * the host fence.  Order to X is preserved because the worker
       * drains in FIFO order. */
      sc_wsi_push(s, flip_idx, wait_fence_fd);
      s->image_index = (s->image_index + 1) % s->wsi_image_count;
   }
   uint64_t g_t3 = NPT_DEBUG(PRESENT_TIMING) ? npt_profile_now_ns() : 0;

   if (NPT_DEBUG(PRESENT_TIMING))
      npt_profile_log_present_timing((unsigned)SyncInterval,
                                     g_t0, g_t1, g_t2, g_t3);

   return hr;
}

/*
 * GetBuffer cache: backbuffers are invariant; (idx, iid) repeats are
 * common.  Cache holds one pub_ref; aux_destroy releases it.  An
 * iid-mismatch on a cached slot is rare and falls through uncached.
 */
static HRESULT NPT_STDMETHODCALLTYPE
sc_GetBuffer_override(void *self, UINT Buffer, REFIID riid, void **ppSurface)
{
   if (!ppSurface)
      return NPT_E_POINTER;
   *ppSurface = NULL;

   if (!riid)
      return NPT_E_POINTER;

   struct npt_dxgi_swapchain_aux *s = sc_aux(self);

   if (Buffer < NPT_SWAPCHAIN_BUFFER_CACHE_MAX) {
      struct npt_swapchain_buffer_cache_entry *slot =
         &s->buffer_cache[Buffer];
      void *cached = atomic_load(&slot->wrapper);
      if (cached && memcmp(&slot->iid, riid, sizeof(GUID)) == 0) {
         npt_com_default_addref(cached);
         *ppSurface = cached;
         return NPT_S_OK;
      }
   }

   void *fresh = NULL;
   HRESULT hr =
      npt_idxgiswapchain_default_GetBuffer(self, Buffer, riid, &fresh);
   if (NPT_FAILED(hr) || !fresh) {
      *ppSurface = fresh;
      return hr;
   }

   if (Buffer >= NPT_SWAPCHAIN_BUFFER_CACHE_MAX) {
      *ppSurface = fresh;
      return hr;
   }

   struct npt_swapchain_buffer_cache_entry *slot = &s->buffer_cache[Buffer];

   /* CAS-publish with an extra cache pub_ref; on race-loss return
    * the winner if the iid matches, NULL otherwise. */
   npt_com_default_addref(fresh);
   void *expected = NULL;
   if (atomic_compare_exchange_strong(&slot->wrapper, &expected, fresh)) {
      slot->iid = *riid;
      atomic_store(&slot->host_id,
                   ((struct npt_com_base *)fresh)->base.id);
      *ppSurface = fresh;
      return hr;
   }

   npt_com_default_release(fresh);
   npt_com_default_release(fresh);

   void *winner = atomic_load(&slot->wrapper);
   if (winner && memcmp(&slot->iid, riid, sizeof(GUID)) == 0) {
      npt_com_default_addref(winner);
      *ppSurface = winner;
   }
   return hr;
}

static void
sc_drop_buffer_cache(struct npt_dxgi_swapchain_aux *s)
{
   struct npt_device *dev = s->com ? s->com->base.device : NULL;
   for (uint32_t i = 0; i < NPT_SWAPCHAIN_BUFFER_CACHE_MAX; i++) {
      void *cached = atomic_exchange(&s->buffer_cache[i].wrapper, NULL);
      uint64_t host_id = atomic_exchange(&s->buffer_cache[i].host_id, 0);
      memset(&s->buffer_cache[i].iid, 0, sizeof(GUID));
      if (!cached)
         continue;
      /* The wrapper may have been force-destroyed independently during
       * device teardown -- the device-level drain loop processes
       * wrappers in arbitrary order.  Verify the cache slot still maps
       * to our wrapper before releasing; if it doesn't, the wrapper is
       * freed memory and releasing it would UAF. */
      if (!dev || !host_id ||
          npt_device_wrapper_cache_is_live(dev, host_id, cached))
         npt_com_default_release(cached);
   }
}

/* Drop the cache BEFORE forwarding: the host retires the old
 * backbuffer set, and a concurrent GetBuffer mid-Resize could
 * otherwise grab a stale wrapper. */
static HRESULT NPT_STDMETHODCALLTYPE
sc_ResizeBuffers_override(void *self, UINT BufferCount, UINT Width,
                          UINT Height, DXGI_FORMAT NewFormat,
                          UINT SwapChainFlags)
{
   struct npt_dxgi_swapchain_aux *s = sc_aux(self);
   sc_drop_buffer_cache(s);
   return npt_idxgiswapchain_default_ResizeBuffers(
      self, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

static HRESULT NPT_STDMETHODCALLTYPE
sc3_ResizeBuffers1_override(void *self, UINT BufferCount, UINT Width,
                            UINT Height, DXGI_FORMAT Format,
                            UINT SwapChainFlags,
                            const UINT *pCreationNodeMask,
                            IUnknown **ppPresentQueue)
{
   struct npt_dxgi_swapchain_aux *s = sc_aux(self);
   sc_drop_buffer_cache(s);
   return npt_idxgiswapchain3_default_ResizeBuffers1(
      self, BufferCount, Width, Height, Format, SwapChainFlags,
      pCreationNodeMask, ppPresentQueue);
}

static void
npt_dxgi_swapchain_aux_init(struct npt_com_base *com,
                            struct npt_device *dev, uint64_t host_id)
{
   struct npt_dxgi_swapchain_aux *s = com->aux;
   s->com = com;
   com->aux_destroy = npt_dxgi_swapchain_aux_destroy;

   mtx_init(&s->wsi_mutex, mtx_plain);
   cnd_init(&s->wsi_cond);
   mtx_init(&s->output_lock, mtx_plain);

   /* Partition the host's sync_queues table: slot 0 is the CPU
    * timeline, [1, present_count + 1) is the Present-fence range,
    * and the upper half is event's domain.  See
    * npt_renderer_event_ring_base in npt_renderer.h. */
   const uint32_t present_count =
      npt_renderer_present_ring_count(&dev->renderer->info);
   uint32_t idx = atomic_fetch_add(&dev->next_present_ring_idx, 1);
   s->host_ring_idx = ((idx - 1) % present_count) + 1u;
   if (idx > present_count)
      npt_log("swapchain ring_idx wrapped to %u (total allocated: %u, cap: %u)",
              s->host_ring_idx, idx, present_count);

   for (uint32_t i = 0; i < NPT_PRESENT_LATENCY; i++)
      s->latency_fences[i] = -1;
   s->latency_count = 0;

   (void)host_id;
}

static void
npt_dxgi_swapchain_aux_destroy(void *aux_raw)
{
   struct npt_dxgi_swapchain_aux *s = aux_raw;
   sc_drop_buffer_cache(s);
   sc_wsi_stop(s);
   sc_forwarder_stop(s);
   /* If sc_wsi_stop skipped its join, the worker is still live with
    * a pointer to this aux.  Leaking the aux (and its mutex / cond /
    * thread handle / host WSI registration) lets the worker finish
    * its in-flight xcb call and exit on its own.  Kernel reaps at
    * process exit. */
   if (s->com && s->com->base.device &&
       npt_device_is_shutting_down(s->com->base.device))
      return;
   cnd_destroy(&s->wsi_cond);
   mtx_destroy(&s->wsi_mutex);
   if (s->fullscreen_target) {
      const struct npt_idxgioutput_client_vtbl *v =
         (const void *)((struct npt_com_base *)s->fullscreen_target)->lpVtbl;
      v->Release(s->fullscreen_target);
   }
   if (s->containing_output) {
      const struct npt_idxgioutput_client_vtbl *v =
         (const void *)((struct npt_com_base *)s->containing_output)->lpVtbl;
      v->Release(s->containing_output);
   }
   mtx_destroy(&s->output_lock);
   for (uint32_t i = 0; i < NPT_PRESENT_LATENCY; i++) {
      if (s->latency_fences[i] >= 0) {
         npt_wine_unixlib_close(s->latency_fences[i]);
         s->latency_fences[i] = -1;
      }
   }
   if (s->wsi_initialized)
      npt_renderer_wsi_destroy(s->com->base.device->renderer);
   free(s);
}

/* =========================================================================
 * IDXGIOutput-related overrides
 *
 * Guest-fab IDXGIOutput wrappers (see npt_overrides_dxgi_output.c) must
 * never reach the host.  Strip pTarget on the way out; serve return-
 * direction outputs from per-swapchain cached state.
 * ========================================================================= */

/* IDXGIOutput is forward-declared only on MinGW (no dxgi.h); reach the
 * vtbl via npt_com_base, which is layout-compatible. */
static inline const struct npt_idxgioutput_client_vtbl *
output_vtbl(IDXGIOutput *o)
{
   return (const void *)((struct npt_com_base *)o)->lpVtbl;
}

static void
sc_aux_assign_output(struct npt_dxgi_swapchain_aux *s,
                     IDXGIOutput **slot, IDXGIOutput *new_val)
{
   IDXGIOutput *old;
   mtx_lock(&s->output_lock);
   old = *slot;
   if (new_val) output_vtbl(new_val)->AddRef(new_val);
   *slot = new_val;
   mtx_unlock(&s->output_lock);
   if (old) output_vtbl(old)->Release(old);
}

static IDXGIOutput *
sc_aux_load_output_addref(struct npt_dxgi_swapchain_aux *s,
                          IDXGIOutput **slot)
{
   IDXGIOutput *v;
   mtx_lock(&s->output_lock);
   v = *slot;
   if (v) output_vtbl(v)->AddRef(v);
   mtx_unlock(&s->output_lock);
   return v;
}

/* Walk swapchain → IDXGIDevice → IDXGIAdapter → EnumOutputs(0).  Each
 * step is a wrapper-cache hit after the first call in the process.  The
 * adapter's EnumOutputs is overridden to return our guest-fab wrappers,
 * so the result is identity-compatible with EnumOutputs(0) callers.
 * Caller owns the returned ref. */
static IDXGIOutput *
resolve_first_output_from_swapchain(void *sc_self)
{
   IDXGIDevice *dev = NULL;
   HRESULT hr = npt_idxgidevicesubobject_default_GetDevice(
      sc_self, &NPT_IID_IDXGIDevice, (void **)&dev);
   if (FAILED(hr) || !dev)
      return NULL;

   IDXGIAdapter *adp = NULL;
   const struct npt_idxgidevice_client_vtbl *dev_vtbl =
      (const struct npt_idxgidevice_client_vtbl *)
         ((struct npt_com_base *)dev)->lpVtbl;
   hr = dev_vtbl->GetAdapter(dev, &adp);
   if (FAILED(hr) || !adp) {
      dev_vtbl->Release(dev);
      return NULL;
   }

   IDXGIOutput *out = NULL;
   const struct npt_idxgiadapter_client_vtbl *adp_vtbl =
      (const struct npt_idxgiadapter_client_vtbl *)
         ((struct npt_com_base *)adp)->lpVtbl;
   hr = adp_vtbl->EnumOutputs(adp, 0, &out);
   adp_vtbl->Release(adp);
   dev_vtbl->Release(dev);
   return SUCCEEDED(hr) ? out : NULL;
}

static HRESULT NPT_STDMETHODCALLTYPE
sc_SetFullscreenState_override(void *self, BOOL Fullscreen, IDXGIOutput *pTarget)
{
   struct npt_dxgi_swapchain_aux *s = sc_aux(self);
   if (s)
      sc_aux_assign_output(s, &s->fullscreen_target,
                           Fullscreen ? pTarget : NULL);
   return npt_idxgiswapchain_default_SetFullscreenState(self, Fullscreen, NULL);
}

static HRESULT NPT_STDMETHODCALLTYPE
sc_GetFullscreenState_override(void *self, BOOL *pFullscreen,
                               IDXGIOutput **ppTarget)
{
   BOOL fs = FALSE;
   /* ppTarget=NULL on the wire: the default thunk skips wrapper
    * construction entirely when the out-pointer is NULL. */
   HRESULT hr = npt_idxgiswapchain_default_GetFullscreenState(self, &fs, NULL);
   if (FAILED(hr)) return hr;
   if (pFullscreen) *pFullscreen = fs;
   if (ppTarget) {
      struct npt_dxgi_swapchain_aux *s = sc_aux(self);
      *ppTarget = (fs && s)
                     ? sc_aux_load_output_addref(s, &s->fullscreen_target)
                     : NULL;
   }
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
sc_GetContainingOutput_override(void *self, IDXGIOutput **ppOutput)
{
   if (!ppOutput) return NPT_E_POINTER;
   struct npt_dxgi_swapchain_aux *s = sc_aux(self);
   if (!s) { *ppOutput = NULL; return NPT_DXGI_ERROR_NOT_FOUND; }

   *ppOutput = sc_aux_load_output_addref(s, &s->containing_output);
   if (*ppOutput) return NPT_S_OK;

   /* assign_output AddRefs into the cache; resolver gave us one ref
    * which we transfer to the caller via *ppOutput. */
   IDXGIOutput *first = resolve_first_output_from_swapchain(self);
   if (!first) return NPT_DXGI_ERROR_NOT_FOUND;
   sc_aux_assign_output(s, &s->containing_output, first);
   *ppOutput = first;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
sc1_GetRestrictToOutput_override(void *self, IDXGIOutput **pp)
{
   (void)self;
   if (!pp) return NPT_E_POINTER;
   *pp = NULL;
   return NPT_S_OK;
}

void
npt_overrides_dxgi_swapchain_init(void)
{
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN (Present,
                                          sc_Present_override);
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN (GetBuffer,
                                          sc_GetBuffer_override);
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN (ResizeBuffers,
                                          sc_ResizeBuffers_override);
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN3(ResizeBuffers1,
                                          sc3_ResizeBuffers1_override);
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN (SetFullscreenState,
                                          sc_SetFullscreenState_override);
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN (GetFullscreenState,
                                          sc_GetFullscreenState_override);
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN (GetContainingOutput,
                                          sc_GetContainingOutput_override);
   NPT_REGISTER_OVERRIDE_DXGI_SWAP_CHAIN1(GetRestrictToOutput,
                                          sc1_GetRestrictToOutput_override);

   npt_com_register_family(swapchain_tiers,
                           sizeof(struct npt_dxgi_swapchain_aux),
                           npt_dxgi_swapchain_aux_init);
}
