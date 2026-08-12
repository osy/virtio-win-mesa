/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-fabricated IDXGISwapChain for the D3D12 (flip-model) Wine path.
 * The pool/worker/backpressure machinery is shared with the D3D11
 * swapchain (npt_swapchain_common.c); this file owns what flip model
 * adds on top.
 *
 * The swapchain never touches the tier default vtbls: it swaps in its
 * own fully-populated vtbl at creation, so no wire method can ever see
 * a guest-fab object id (an unregistered id is a fatal host decode
 * error).  Model:
 *
 *   backbuffers     desc.BufferCount committed DEFAULT-heap host
 *                   textures with REAL flip-model rotation:
 *                   GetBuffer(i) returns buffer i and
 *                   GetCurrentBackBufferIndex() = present_count %
 *                   BufferCount.
 *   present pool    NPT_SC_PRESENT_IMAGES committed host textures
 *                   created with HEAP_FLAG_SHARED plus the host's
 *                   EXPORT_LINEAR_DMABUF bit, exported once via
 *                   the SHARED transport, bound to guest blobs whose
 *                   dmabufs feed the X11 WSI.
 *   copy lists      BufferCount x pool pre-recorded CLOSED command
 *                   lists (barriers + CopyResource(pool[i], bb[j]) +
 *                   barriers), recorded once at WSI init and
 *                   re-executed every Present (closed lists are
 *                   re-executable; no allocator-reset dance).
 *   Present         ExecuteCommandLists(copy list) on the app's queue
 *                   (through the queue wrapper's vtbl so the ECL
 *                   override's TLS-ring drain fires), then
 *                   queue->Signal(fence, ++n) (async wire), then a
 *                   fresh event token armed into a guest sync_file
 *                   (npt_event_arm_token_fd) + async
 *                   fence->SetEventOnCompletion(n, token).  The
 *                   sync_file is handed to the shared WSI worker, so
 *                   the app's thread never blocks on the host GPU.
 */

#include "npt_swapchain12.h"

#include "npt_com.h"
#include "npt_device.h"
#include "npt_env.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "npt_ring.h"
#include "npt_shared_texture.h"
#include "npt_swapchain_common.h"

#include <string.h>

#include "neptune-protocol/npt_protocol_client_idxgiswapchain.h"

#if defined(__WINE__)

#include "npt_event.h"
#include "npt_renderer_wine_common.h"
#include "nptunix/npt_unixlib.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <windows.h>

#include "neptune-protocol/npt_protocol_client_id3d12commandqueue.h"
#include "neptune-protocol/npt_protocol_client_id3d12device.h"
#include "neptune-protocol/npt_protocol_client_id3d12fence.h"
#include "neptune-protocol/npt_protocol_client_id3d12graphicscommandlist.h"
#include "neptune-protocol/npt_protocol_client_idxgiadapter.h"
#include "neptune-protocol/npt_protocol_client_idxgifactory.h"
#include "neptune-protocol/npt_protocol_client_idxgioutput.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_guest_id3d12fence.h"

/* Host vendor D3D12_HEAP_FLAGS bit: forces a linear tiling and modifier
 * on the exported image so modifier-less consumers (QEMU scanout, DRI3
 * pixmaps) can import it.  D3D12 analog of
 * NPT_D3D11_MISC_LINEAR_EXPORT. */
#define NPT_D3D12_HEAP_FLAG_EXPORT_LINEAR_DMABUF 0x40000000u

struct npt_guest_swapchain12 {
   struct npt_guest_swapchain_common base;

   /* Held wire wrappers (one public ref each) + their host ids for
    * liveness-guarded release at teardown (see npt_gsc_release_held). */
   void *factory;                /* creating IDXGIFactory*, may be NULL */
   uint64_t factory_id;
   void *queue;                  /* ID3D12CommandQueue* */
   uint64_t queue_id;
   void *device;                 /* ID3D12Device* (via queue GetDevice) */
   uint64_t device_id;

   /* Internal GPU-done fence: Signal on the queue after each present
    * copy; per-Present event tokens turn its completion into a guest
    * sync_file (see npt_event_arm_token_fd).  The blocking
    * SetEventOnCompletion(value, NULL) form remains for the rare full
    * drains (resize, teardown). */
   void *fence;                  /* ID3D12Fence* */
   uint64_t fence_id;
   uint64_t fence_value;

   /* Flip-model backbuffers (real rotation). */
   void *backbuffer[NPT_SC_MAX_SWAP_CHAIN_BUFFERS];
   uint64_t backbuffer_id[NPT_SC_MAX_SWAP_CHAIN_BUFFERS];

   /* Pre-recorded copy lists (see file comment). */
   void *copy_allocator;         /* ID3D12CommandAllocator* (DIRECT) */
   uint64_t copy_allocator_id;
   void *copy_list[NPT_SC_MAX_SWAP_CHAIN_BUFFERS][NPT_SC_PRESENT_IMAGES];
   uint64_t copy_list_id[NPT_SC_MAX_SWAP_CHAIN_BUFFERS][NPT_SC_PRESENT_IMAGES];
};

static inline struct npt_guest_swapchain12 *
gsc12(void *self)
{
   return ((struct npt_com_base *)self)->aux;
}

static inline const struct npt_id3d12device_client_vtbl *
gsc12_device_vtbl(struct npt_guest_swapchain12 *s)
{
   return (const void *)((struct npt_com_base *)s->device)->lpVtbl;
}

static inline const struct npt_id3d12commandqueue_client_vtbl *
gsc12_queue_vtbl(struct npt_guest_swapchain12 *s)
{
   return (const void *)((struct npt_com_base *)s->queue)->lpVtbl;
}

/* ------------------------------------------------------------------ */
/* GPU sync (full drains: resize + teardown)                           */
/* ------------------------------------------------------------------ */

/* Signal the internal fence on the queue and block until the host GPU
 * has retired everything submitted so far.  The NULL-event
 * SetEventOnCompletion form is a synchronous wire round trip, with the
 * fence override routing it sync and the host doing the waiting. */
static void
gsc12_gpu_drain(struct npt_guest_swapchain12 *s)
{
   if (!s->queue || !s->fence)
      return;
   const uint64_t value = ++s->fence_value;
   HRESULT hr = gsc12_queue_vtbl(s)->Signal(s->queue,
                                            (ID3D12Fence *)s->fence, value);
   if (NPT_FAILED(hr)) {
      npt_log("guest swapchain12: queue Signal(%llu) failed hr=0x%x",
              (unsigned long long)value, hr);
      return;
   }
   const struct npt_id3d12fence_client_vtbl *fv =
      (const void *)((struct npt_com_base *)s->fence)->lpVtbl;
   hr = fv->SetEventOnCompletion(s->fence, value, NULL);
   if (NPT_FAILED(hr))
      npt_log("guest swapchain12: fence wait(%llu) failed hr=0x%x",
              (unsigned long long)value, hr);
}

/* ------------------------------------------------------------------ */
/* backbuffers                                                         */
/* ------------------------------------------------------------------ */

static void
gsc12_release_backbuffers(struct npt_guest_swapchain12 *s,
                          struct npt_device *dev)
{
   for (uint32_t i = 0; i < NPT_SC_MAX_SWAP_CHAIN_BUFFERS; i++)
      npt_gsc_release_held(dev, &s->backbuffer[i], &s->backbuffer_id[i]);
}

static HRESULT
gsc12_create_backbuffers(struct npt_guest_swapchain12 *s)
{
   struct npt_guest_swapchain_common *c = &s->base;
   D3D12_HEAP_PROPERTIES hp;
   memset(&hp, 0, sizeof(hp));
   hp.Type = D3D12_HEAP_TYPE_DEFAULT;

   D3D12_RESOURCE_DESC rd;
   memset(&rd, 0, sizeof(rd));
   rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   rd.Width = c->desc.Width;
   rd.Height = c->desc.Height;
   rd.DepthOrArraySize = 1;
   rd.MipLevels = 1;
   rd.Format = c->desc.Format;
   rd.SampleDesc = c->desc.SampleDesc;
   rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
   rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

   const struct npt_id3d12device_client_vtbl *dv = gsc12_device_vtbl(s);
   for (uint32_t i = 0; i < c->desc.BufferCount; i++) {
      void *buf = NULL;
      /* Apps expect backbuffers in PRESENT state (== COMMON, value 0). */
      HRESULT hr = dv->CreateCommittedResource(
         s->device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
         D3D12_RESOURCE_STATE_PRESENT, NULL,
         &NPT_IID_ID3D12Resource, &buf);
      if (NPT_FAILED(hr) || !buf) {
         npt_log("guest swapchain12: backbuffer %u/%u %ux%u fmt=%u failed "
                 "hr=0x%x", i, c->desc.BufferCount, (unsigned)rd.Width,
                 rd.Height, rd.Format, hr);
         gsc12_release_backbuffers(s, c->com->base.device);
         return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
      }
      s->backbuffer[i] = buf;
      s->backbuffer_id[i] = npt_com_self_id(buf);
   }
   return NPT_S_OK;
}

/* ------------------------------------------------------------------ */
/* present pool + copy lists + WSI                                     */
/* ------------------------------------------------------------------ */

/* Exportable D3D12 present-pool texture: committed DEFAULT texture on a
 * SHARED heap with the EXPORT_LINEAR_DMABUF bit, in COMMON state (the
 * copy lists transition it).  D3D12 analog of
 * npt_shared_texture_create_exportable. */
static void *
gsc12_create_pool_texture(struct npt_guest_swapchain12 *s, uint32_t fmt)
{
   struct npt_guest_swapchain_common *c = &s->base;
   D3D12_HEAP_PROPERTIES hp;
   memset(&hp, 0, sizeof(hp));
   hp.Type = D3D12_HEAP_TYPE_DEFAULT;

   D3D12_RESOURCE_DESC rd;
   memset(&rd, 0, sizeof(rd));
   rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   rd.Width = c->desc.Width;
   rd.Height = c->desc.Height;
   rd.DepthOrArraySize = 1;
   rd.MipLevels = 1;
   rd.Format = (DXGI_FORMAT)fmt;
   rd.SampleDesc.Count = 1;
   rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
   rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

   const D3D12_HEAP_FLAGS heap_flags = (D3D12_HEAP_FLAGS)
      (D3D12_HEAP_FLAG_SHARED | NPT_D3D12_HEAP_FLAG_EXPORT_LINEAR_DMABUF);

   void *tex = NULL;
   HRESULT hr = gsc12_device_vtbl(s)->CreateCommittedResource(
      s->device, &hp, heap_flags, &rd, D3D12_RESOURCE_STATE_COMMON, NULL,
      &NPT_IID_ID3D12Resource, &tex);
   if (NPT_FAILED(hr) || !tex) {
      npt_log("guest swapchain12: exportable %ux%u fmt=%u create failed "
              "hr=0x%x", (unsigned)rd.Width, rd.Height, fmt, hr);
      return NULL;
   }
   return tex;
}

static void
gsc12_release_copy_lists(struct npt_guest_swapchain12 *s,
                         struct npt_device *dev)
{
   for (uint32_t j = 0; j < NPT_SC_MAX_SWAP_CHAIN_BUFFERS; j++)
      for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++)
         npt_gsc_release_held(dev, &s->copy_list[j][i],
                              &s->copy_list_id[j][i]);
   npt_gsc_release_held(dev, &s->copy_allocator, &s->copy_allocator_id);
}

/* Record all BufferCount x pool copy lists, closed.  One allocator
 * backs them all: only one list is ever recording at a time (created,
 * recorded, closed sequentially), and closed lists are re-executable
 * without an allocator reset. */
static bool
gsc12_record_copy_lists(struct npt_guest_swapchain12 *s)
{
   struct npt_guest_swapchain_common *c = &s->base;
   const struct npt_id3d12device_client_vtbl *dv = gsc12_device_vtbl(s);

   void *alloc = NULL;
   HRESULT hr = dv->CreateCommandAllocator(
      s->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
      &NPT_IID_ID3D12CommandAllocator, &alloc);
   if (NPT_FAILED(hr) || !alloc) {
      npt_log("guest swapchain12: copy allocator failed hr=0x%x", hr);
      return false;
   }
   s->copy_allocator = alloc;
   s->copy_allocator_id = npt_com_self_id(alloc);

   for (uint32_t j = 0; j < c->desc.BufferCount; j++) {
      for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
         void *list = NULL;
         hr = dv->CreateCommandList(
            s->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            (ID3D12CommandAllocator *)alloc, NULL,
            &NPT_IID_ID3D12GraphicsCommandList, &list);
         if (NPT_FAILED(hr) || !list) {
            npt_log("guest swapchain12: copy list [%u][%u] failed hr=0x%x",
                    j, i, hr);
            return false;
         }
         s->copy_list[j][i] = list;
         s->copy_list_id[j][i] = npt_com_self_id(list);

         const struct npt_id3d12graphicscommandlist_client_vtbl *lv =
            (const void *)((struct npt_com_base *)list)->lpVtbl;

         D3D12_RESOURCE_BARRIER b[2];
         memset(b, 0, sizeof(b));
         b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
         b[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
         b[0].Transition.pResource = (ID3D12Resource *)s->backbuffer[j];
         b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
         b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
         b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
         b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
         b[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
         b[1].Transition.pResource = (ID3D12Resource *)c->present_tex[i];
         b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
         b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
         b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
         lv->ResourceBarrier(list, 2, b);

         lv->CopyResource(list, (ID3D12Resource *)c->present_tex[i],
                          (ID3D12Resource *)s->backbuffer[j]);

         /* Return both to their steady states so the list is
          * re-executable as-is. */
         b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
         b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
         b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
         b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
         lv->ResourceBarrier(list, 2, b);

         hr = lv->Close(list);
         if (NPT_FAILED(hr)) {
            npt_log("guest swapchain12: copy list [%u][%u] Close failed "
                    "hr=0x%x", j, i, hr);
            return false;
         }
      }
   }
   return true;
}

static void
gsc12_teardown_wsi(struct npt_guest_swapchain12 *s, bool shutting_down)
{
   struct npt_guest_swapchain_common *c = &s->base;
   npt_gsc_wsi_quiesce(c, shutting_down);
   if (!shutting_down) {
      gsc12_release_copy_lists(s, c->com->base.device);
      npt_gsc_release_pool(c);
   }
}

/* First Present: build the exportable pool, hand its dmabufs to the
 * WSI backend, and pre-record the copy lists.  The window must exist
 * by now (the app rendered a frame), which is why this is not done at
 * creation time. */
static void
gsc12_ensure_wsi(struct npt_guest_swapchain12 *s)
{
   struct npt_guest_swapchain_common *c = &s->base;

   if (!npt_gsc_wsi_backend_available(c))
      return;

   const uint32_t fmt = npt_shared_texture_host_format(c->desc.Format);
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      void *tex = gsc12_create_pool_texture(s, fmt);
      if (!tex) {
         npt_gsc_release_pool(c);
         c->wsi_failed = true;
         return;
      }
      c->present_tex[i] = tex;
      c->present_tex_id[i] = npt_com_self_id(tex);
   }

   if (!npt_gsc_wsi_bringup(c, fmt))
      return;

   /* The lists only reference the pool and the backbuffers; the (just
    * started) workers act on nothing until the first push, so recording
    * after bring-up is safe. */
   if (!gsc12_record_copy_lists(s)) {
      gsc12_teardown_wsi(s, false);
      c->wsi_failed = true;
      return;
   }

   npt_log("guest swapchain12: WSI up %ux%u fmt=%u buffers=%u pool=%u",
           c->desc.Width, c->desc.Height, fmt, c->desc.BufferCount,
           NPT_SC_PRESENT_IMAGES);
}

/* ------------------------------------------------------------------ */
/* IDXGIObject / IDXGIDeviceSubObject                                  */
/* ------------------------------------------------------------------ */

static HRESULT NPT_STDMETHODCALLTYPE
gsc12_GetParent(void *self, const IID *riid, void **ppParent)
{
   struct npt_guest_swapchain12 *s = gsc12(self);
   if (s->factory)
      return npt_gsc_qi_held(s->factory, riid, ppParent);
   if (!ppParent)
      return NPT_E_POINTER;
   *ppParent = NULL;
   return NPT_E_FAIL;
}

/* DXGI GetDevice hands back whatever the swapchain was created from
 * that matches riid; try the D3D12 device first, then the queue. */
static HRESULT NPT_STDMETHODCALLTYPE
gsc12_GetDevice(void *self, const IID *riid, void **ppDevice)
{
   struct npt_guest_swapchain12 *s = gsc12(self);
   HRESULT hr = npt_gsc_qi_held(s->device, riid, ppDevice);
   if (NPT_FAILED(hr))
      hr = npt_gsc_qi_held(s->queue, riid, ppDevice);
   return hr;
}

/* ------------------------------------------------------------------ */
/* Present                                                             */
/* ------------------------------------------------------------------ */

static HRESULT
gsc12_present_common(void *self, UINT SyncInterval, UINT Flags)
{
   struct npt_guest_swapchain12 *s = gsc12(self);
   struct npt_guest_swapchain_common *c = &s->base;

   if (Flags & NPT_DXGI_PRESENT_TEST)
      return (c->wsi_failed || c->occluded_streak >= NPT_SC_OCCLUDED_STREAK)
                ? NPT_DXGI_STATUS_OCCLUDED : NPT_S_OK;

   /* Frame-time instrumentation (NPT_DEBUG=present_timing).  t0=entry,
    * t1=after backpressure, t2=after the host present work is issued,
    * t3=after the WSI flip is queued. */
   const bool prof = NPT_DEBUG(PRESENT_TIMING);
   uint64_t t0 = prof ? npt_profile_now_ns() : 0, t1 = t0, t2 = t0, t3 = t0;

   mtx_lock(&c->present_mutex);

   if (!c->wsi_initialized && !c->wsi_failed)
      gsc12_ensure_wsi(s);
   if (c->wsi_failed) {
      c->present_count++;
      mtx_unlock(&c->present_mutex);
      /* No worker will complete this frame; release the latency object
       * synchronously so a waiting app is not stalled. */
      npt_gsc_frame_latency_release(c);
      return NPT_S_OK;
   }

   struct npt_device *dev = c->com->base.device;

   /* CPU-runahead backpressure before issuing more work: blocks the
    * caller until the host GPU has progressed.  Combined with the
    * async WSI worker this bounds runahead at max_frame_latency
    * frames without the app thread ever polling a fence itself. */
   npt_gsc_backpressure_wait(c);
   if (prof) t1 = npt_profile_now_ns();

   const uint32_t bb = c->present_count % c->desc.BufferCount;
   const uint32_t img = c->present_count % NPT_SC_PRESENT_IMAGES;
   npt_gsc_reuse_gate(c, img);

   /* Through the queue wrapper's vtbl slot so the ECL override's
    * TLS-ring drain fires before the submission dispatches. */
   ID3D12CommandList *list = (ID3D12CommandList *)s->copy_list[bb][img];
   gsc12_queue_vtbl(s)->ExecuteCommandLists(s->queue, 1, &list);

   /* GPU-done fence: Signal on the queue after the copy (async wire;
    * the queue ring keeps it ordered after the ECL), then arm a fresh
    * event token on its completion value and turn that into a guest
    * sync_file the WSI worker feeds to xcb_present as the wait_fence.
    * Without it the X server can read the dmabuf while the host GPU
    * is still writing (cross-VM dmabuf has no implicit reservation
    * sync). */
   const uint64_t value = ++s->fence_value;
   gsc12_queue_vtbl(s)->Signal(s->queue, (ID3D12Fence *)s->fence, value);

   uint64_t token = npt_gsc_make_token();
   /* REGISTER + ARM (synchronous) must land before the host decodes
    * SetEventOnCompletion below: its hEvent -> eventfd substitution
    * only fires once the token's proxy exists (else the raw token
    * passes through as a bogus HANDLE).  Bypass the fence override
    * (it treats hEvent as a real Win32 event). */
   int wait_fence_fd = npt_event_arm_token_fd(dev, token);
   if (wait_fence_fd < 0) {
      token = 0;  /* arm failed: self-released; present unfenced */
   } else {
      /* Order vs the queue-ring Signal is immaterial: the host fence
       * worker fires immediately if the value is already reached. */
      npt_async_ID3D12Fence_SetEventOnCompletion(
         npt_device_method_ring(dev), s->fence_id, value,
         (HANDLE)(uintptr_t)token);
      /* Dup for the latency ring before the worker consumes the
       * original; a token can't be re-armed, so dup rather than
       * re-arm. */
      npt_gsc_backpressure_record(c, npt_wine_unixlib_dup(wait_fence_fd));
   }
   if (prof) t2 = npt_profile_now_ns();

   atomic_fetch_add(&c->image_inflight[img], 1);
   npt_gsc_wsi_push(c, img, wait_fence_fd, token);

   c->present_count++;
   npt_profile_present_marker();
   if (prof) {
      t3 = npt_profile_now_ns();
      npt_profile_log_present_timing((unsigned)SyncInterval, t0, t1, t2, t3);
   }
   const bool occluded = c->occluded_streak >= NPT_SC_OCCLUDED_STREAK;
   mtx_unlock(&c->present_mutex);
   return occluded ? NPT_DXGI_STATUS_OCCLUDED : NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc12_Present(void *self, UINT SyncInterval, UINT Flags)
{
   return gsc12_present_common(self, SyncInterval, Flags);
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc12_Present1(void *self, UINT SyncInterval, UINT PresentFlags,
               const DXGI_PRESENT_PARAMETERS *pPresentParameters)
{
   if (!pPresentParameters)
      return NPT_DXGI_ERROR_INVALID_CALL;
   /* Dirty/scroll rects are an optimization hint; full-frame present
    * is always correct. */
   return gsc12_present_common(self, SyncInterval, PresentFlags);
}

/* ------------------------------------------------------------------ */
/* buffers / desc                                                      */
/* ------------------------------------------------------------------ */

static HRESULT NPT_STDMETHODCALLTYPE
gsc12_GetBuffer(void *self, UINT Buffer, const IID *riid, void **ppSurface)
{
   struct npt_guest_swapchain12 *s = gsc12(self);
   struct npt_guest_swapchain_common *c = &s->base;
   if (!ppSurface)
      return NPT_E_POINTER;
   *ppSurface = NULL;
   if (Buffer >= c->desc.BufferCount)
      return NPT_DXGI_ERROR_INVALID_CALL;
   mtx_lock(&c->present_mutex);
   void *bb = s->backbuffer[Buffer];
   HRESULT hr = npt_gsc_qi_held(bb, riid, ppSurface);
   mtx_unlock(&c->present_mutex);
   return hr;
}

static UINT NPT_STDMETHODCALLTYPE
gsc12_GetCurrentBackBufferIndex(void *self)
{
   struct npt_guest_swapchain12 *s = gsc12(self);
   struct npt_guest_swapchain_common *c = &s->base;
   return c->desc.BufferCount ? c->present_count % c->desc.BufferCount : 0;
}

static HRESULT
gsc12_resize_common(void *self, UINT BufferCount, UINT Width, UINT Height,
                    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
   struct npt_guest_swapchain12 *s = gsc12(self);
   struct npt_guest_swapchain_common *c = &s->base;
   struct npt_device *dev = c->com->base.device;
   (void)SwapChainFlags;

   if (BufferCount > NPT_SC_MAX_SWAP_CHAIN_BUFFERS ||
       BufferCount == 1 /* flip model needs >= 2; 0 keeps current */)
      return NPT_DXGI_ERROR_INVALID_CALL;

   mtx_lock(&c->present_mutex);

   /* Full GPU drain, then drop the pool + copy lists + X pixmaps;
    * everything is rebuilt lazily on the next Present. */
   if (c->wsi_initialized)
      gsc12_gpu_drain(s);
   gsc12_teardown_wsi(s, false);

   if (BufferCount)
      c->desc.BufferCount = BufferCount;
   if (NewFormat != 0 /* DXGI_FORMAT_UNKNOWN */)
      c->desc.Format = NewFormat;
   if (Width && Height) {
      c->desc.Width = Width;
      c->desc.Height = Height;
   } else {
      UINT w = Width, h = Height;
      npt_gsc_query_window_size(c, &w, &h);
      c->desc.Width = w;
      c->desc.Height = h;
   }

   /* DXGI requires the app to have dropped its buffer refs; be lenient
    * (refcounting keeps a straggler alive) but recreate. */
   gsc12_release_backbuffers(s, dev);
   HRESULT hr = gsc12_create_backbuffers(s);

   mtx_unlock(&c->present_mutex);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc12_ResizeBuffers(void *self, UINT BufferCount, UINT Width, UINT Height,
                    DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
   return gsc12_resize_common(self, BufferCount, Width, Height, NewFormat,
                              SwapChainFlags);
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc12_ResizeBuffers1(void *self, UINT BufferCount, UINT Width, UINT Height,
                     DXGI_FORMAT Format, UINT SwapChainFlags,
                     const UINT *pCreationNodeMask, IUnknown **ppPresentQueue)
{
   (void)pCreationNodeMask; (void)ppPresentQueue;
   return gsc12_resize_common(self, BufferCount, Width, Height, Format,
                              SwapChainFlags);
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc12_ResizeTarget(void *self, const DXGI_MODE_DESC *pNewTargetParameters)
{
   if (!pNewTargetParameters)
      return NPT_DXGI_ERROR_INVALID_CALL;
   struct npt_guest_swapchain12 *s = gsc12(self);
   struct npt_guest_swapchain_common *c = &s->base;
   HWND hwnd = c->hwnd;
   if (!hwnd || !IsWindow(hwnd))
      return NPT_DXGI_ERROR_INVALID_CALL;
   /* Windowed client-area resize only (fullscreen is a stored flag;
    * no guest-side modesetting yet). */
   if (!c->fullscreen && pNewTargetParameters->Width &&
       pNewTargetParameters->Height)
      npt_gsc_window_resize(hwnd, pNewTargetParameters->Width,
                            pNewTargetParameters->Height);
   return NPT_S_OK;
}

/* ------------------------------------------------------------------ */
/* fullscreen / outputs                                                */
/* ------------------------------------------------------------------ */

/* Fullscreen is a stored flag only: the WSI presents to the app's
 * window either way. */
static HRESULT NPT_STDMETHODCALLTYPE
gsc12_SetFullscreenState(void *self, BOOL Fullscreen, IDXGIOutput *pTarget)
{
   struct npt_guest_swapchain12 *s = gsc12(self);
   if (!Fullscreen && pTarget)
      return NPT_DXGI_ERROR_INVALID_CALL;
   s->base.fullscreen = Fullscreen;
   s->base.fs_desc.Windowed = !Fullscreen;
   return NPT_S_OK;
}

/* factory -> EnumAdapters(0) -> EnumOutputs(0); the adapter override
 * fabricates the output, so the result is identity-compatible with
 * EnumOutputs(0) callers.  Caller owns the returned ref. */
static IDXGIOutput *
gsc12_resolve_first_output(struct npt_guest_swapchain12 *s)
{
   if (!s->factory)
      return NULL;
   const struct npt_idxgifactory_client_vtbl *fv =
      (const void *)((struct npt_com_base *)s->factory)->lpVtbl;

   IDXGIAdapter *adp = NULL;
   HRESULT hr = fv->EnumAdapters(s->factory, 0, &adp);
   if (NPT_FAILED(hr) || !adp)
      return NULL;
   const struct npt_idxgiadapter_client_vtbl *av =
      (const void *)((struct npt_com_base *)adp)->lpVtbl;

   IDXGIOutput *out = NULL;
   hr = av->EnumOutputs(adp, 0, &out);
   av->Release(adp);
   return NPT_SUCCEEDED(hr) ? out : NULL;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc12_GetContainingOutput(void *self, IDXGIOutput **ppOutput)
{
   struct npt_guest_swapchain12 *s = gsc12(self);
   struct npt_guest_swapchain_common *c = &s->base;
   if (!ppOutput)
      return NPT_E_POINTER;

   *ppOutput = npt_gsc_load_output_addref(c, &c->containing_output);
   if (*ppOutput)
      return NPT_S_OK;

   IDXGIOutput *first = gsc12_resolve_first_output(s);
   if (!first)
      return NPT_DXGI_ERROR_NOT_FOUND;
   /* assign_output AddRefs into the cache; the resolver's ref
    * transfers to the caller. */
   npt_gsc_assign_output(c, &c->containing_output, first);
   *ppOutput = first;
   return NPT_S_OK;
}

/* ------------------------------------------------------------------ */
/* vtbl                                                                */
/* ------------------------------------------------------------------ */

static const struct npt_idxgiswapchain4_client_vtbl gsc12_vtbl = {
   .QueryInterface = npt_gsc_QueryInterface,
   .AddRef = npt_gsc_AddRef,
   .Release = npt_gsc_Release,
   .SetPrivateData = npt_gsc_SetPrivateData,
   .SetPrivateDataInterface = npt_gsc_SetPrivateDataInterface,
   .GetPrivateData = npt_gsc_GetPrivateData,
   .GetParent = gsc12_GetParent,
   .GetDevice = gsc12_GetDevice,
   .Present = gsc12_Present,
   .GetBuffer = gsc12_GetBuffer,
   .SetFullscreenState = gsc12_SetFullscreenState,
   .GetFullscreenState = npt_gsc_GetFullscreenState,
   .GetDesc = npt_gsc_GetDesc,
   .ResizeBuffers = gsc12_ResizeBuffers,
   .ResizeTarget = gsc12_ResizeTarget,
   .GetContainingOutput = gsc12_GetContainingOutput,
   .GetFrameStatistics = npt_gsc_GetFrameStatistics,
   .GetLastPresentCount = npt_gsc_GetLastPresentCount,
   .GetDesc1 = npt_gsc_GetDesc1,
   .GetFullscreenDesc = npt_gsc_GetFullscreenDesc,
   .GetHwnd = npt_gsc_GetHwnd,
   .GetCoreWindow = npt_gsc_GetCoreWindow,
   .Present1 = gsc12_Present1,
   .IsTemporaryMonoSupported = npt_gsc_IsTemporaryMonoSupported,
   .GetRestrictToOutput = npt_gsc_GetRestrictToOutput,
   .SetBackgroundColor = npt_gsc_SetBackgroundColor,
   .GetBackgroundColor = npt_gsc_GetBackgroundColor,
   .SetRotation = npt_gsc_SetRotation,
   .GetRotation = npt_gsc_GetRotation,
   .SetSourceSize = npt_gsc_SetSourceSize,
   .GetSourceSize = npt_gsc_GetSourceSize,
   .SetMaximumFrameLatency = npt_gsc_SetMaximumFrameLatency,
   .GetMaximumFrameLatency = npt_gsc_GetMaximumFrameLatency,
   .GetFrameLatencyWaitableObject = npt_gsc_GetFrameLatencyWaitableObject,
   .SetMatrixTransform = npt_gsc_SetMatrixTransform,
   .GetMatrixTransform = npt_gsc_GetMatrixTransform,
   .GetCurrentBackBufferIndex = gsc12_GetCurrentBackBufferIndex,
   .CheckColorSpaceSupport = npt_gsc_CheckColorSpaceSupport,
   .SetColorSpace1 = npt_gsc_SetColorSpace1,
   .ResizeBuffers1 = gsc12_ResizeBuffers1,
   .SetHDRMetaData = npt_gsc_SetHDRMetaData,
};

/* ------------------------------------------------------------------ */
/* creation / destruction                                              */
/* ------------------------------------------------------------------ */

static void
gsc12_aux_destroy(void *aux)
{
   struct npt_guest_swapchain12 *s = aux;
   struct npt_guest_swapchain_common *c = &s->base;
   struct npt_device *dev = c->com ? c->com->base.device : NULL;
   const bool shutting_down = dev && npt_device_is_shutting_down(dev);

   /* Copy lists may still be in flight on the host GPU; releasing an
    * executing list is UB, so drain first (rare path). */
   if (c->wsi_initialized && !shutting_down)
      gsc12_gpu_drain(s);

   gsc12_teardown_wsi(s, shutting_down);

   if (shutting_down) {
      /* Device teardown frees wrappers via its own drain; leak the aux
       * bookkeeping rather than touch freed wrappers (the skipped-join
       * workers may also still run with a pointer to this aux). */
      return;
   }

   npt_gsc_release_outputs(c);

   gsc12_release_backbuffers(s, dev);
   npt_gsc_release_held(dev, &s->fence, &s->fence_id);
   npt_gsc_release_held(dev, &s->device, &s->device_id);
   npt_gsc_release_held(dev, &s->queue, &s->queue_id);
   npt_gsc_release_held(dev, &s->factory, &s->factory_id);

   npt_gsc_fini(c);
   free(s);
}

HRESULT
npt_guest_swapchain12_create(void *queue_unknown,
                             const DXGI_SWAP_CHAIN_DESC1 *desc1,
                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
                             HWND hwnd,
                             void *factory_wrapper,
                             void **out_swapchain)
{
   if (!out_swapchain)
      return NPT_E_POINTER;
   *out_swapchain = NULL;
   if (!queue_unknown || !desc1)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if (!hwnd) {
      npt_log("guest swapchain12: no output window");
      return NPT_DXGI_ERROR_INVALID_CALL;
   }

   /* DXGI flip-model rules: no MSAA and at least two buffers. */
   if (desc1->SampleDesc.Count > 1) {
      npt_log("guest swapchain12: SampleDesc.Count=%u rejected (flip model)",
              desc1->SampleDesc.Count);
      return NPT_DXGI_ERROR_INVALID_CALL;
   }
   if (desc1->BufferCount < 2 ||
       desc1->BufferCount > NPT_SC_MAX_SWAP_CHAIN_BUFFERS) {
      npt_log("guest swapchain12: BufferCount=%u rejected (flip model)",
              desc1->BufferCount);
      return NPT_DXGI_ERROR_INVALID_CALL;
   }

   struct npt_device *dev = npt_com_self_device(queue_unknown);
   if (!dev)
      return NPT_E_FAIL;

   /* The caller's pDevice may be any interface of the D3D12 queue;
    * resolve the ID3D12CommandQueue wrapper we present through (its
    * instance ring keeps present traffic host-serialized with the
    * app's own queue traffic). */
   void *queue = NULL;
   {
      const struct npt_idxgiswapchain_client_vtbl *v =
         (const void *)((struct npt_com_base *)queue_unknown)->lpVtbl;
      HRESULT hr = v->QueryInterface(queue_unknown,
                                     &NPT_IID_ID3D12CommandQueue, &queue);
      if (NPT_FAILED(hr) || !queue)
         return NPT_DXGI_ERROR_INVALID_CALL;
   }

   /* The device that owns the queue: all creates go through it. */
   void *device = NULL;
   {
      const struct npt_id3d12commandqueue_client_vtbl *qv =
         (const void *)((struct npt_com_base *)queue)->lpVtbl;
      HRESULT hr = qv->GetDevice(queue, &NPT_IID_ID3D12Device, &device);
      if (NPT_FAILED(hr) || !device) {
         npt_log("guest swapchain12: queue GetDevice failed hr=0x%x", hr);
         npt_com_default_release(queue);
         return NPT_E_FAIL;
      }
   }

   const uint64_t guest_id = npt_com_make_guest_id(
      NPT_GUEST_KIND_SWAPCHAIN, npt_com_allocate_next_id());
   struct npt_com_base *com =
      npt_com_get_or_wrap(dev, &NPT_IID_IDXGISwapChain4, guest_id,
                          (struct npt_com_base *)device);
   if (!com) {
      npt_com_default_release(device);
      npt_com_default_release(queue);
      return NPT_E_OUTOFMEMORY;
   }

   /* The drain and WSI workers run detached and dereference the device and its
    * renderer, so both must outlive this swapchain.  Today that holds only as a
    * side effect of the wrapper graph pinning the device wrapper; make it a
    * reference this object owns, so the device cannot be destroyed underneath a
    * live swapchain no matter how the graph is refactored. */
   npt_device_acquire();
   atomic_fetch_add_explicit(&com->base.device_ref_holds, 1,
                             memory_order_relaxed);

   struct npt_guest_swapchain12 *s = calloc(1, sizeof(*s));
   if (!s) {
      npt_com_default_release(com);
      npt_com_default_release(device);
      npt_com_default_release(queue);
      return NPT_E_OUTOFMEMORY;
   }

   /* Not published to any other thread yet; safe to swap the vtbl and
    * attach aux without ordering concerns. */
   com->aux = s;
   com->aux_destroy = gsc12_aux_destroy;
   com->lpVtbl = (const void **)&gsc12_vtbl;

   npt_gsc_init(&s->base, com, desc1, fs_desc, hwnd, "guest swapchain12");

   /* Ownership of the resolved refs transfers to the swapchain. */
   s->queue = queue;
   s->queue_id = npt_com_self_id(queue);
   s->device = device;
   s->device_id = npt_com_self_id(device);

   if (factory_wrapper) {
      npt_com_default_addref(factory_wrapper);
      s->factory = factory_wrapper;
      s->factory_id = npt_com_self_id(factory_wrapper);
   }

   /* Internal GPU-done fence; every Present signals it and turns the
    * completion into the WSI wait fence. */
   {
      void *fence = NULL;
      HRESULT hr = gsc12_device_vtbl(s)->CreateFence(
         s->device, 0, D3D12_FENCE_FLAG_NONE, &NPT_IID_ID3D12Fence, &fence);
      if (NPT_FAILED(hr) || !fence) {
         npt_log("guest swapchain12: CreateFence failed hr=0x%x", hr);
         npt_com_default_release(com);
         return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
      }
      s->fence = fence;
      s->fence_id = npt_com_self_id(fence);
   }

   HRESULT hr = gsc12_create_backbuffers(s);
   if (NPT_FAILED(hr)) {
      npt_com_default_release(com);
      return hr;
   }

   npt_log("guest swapchain12: created %ux%u fmt=%u buffers=%u hwnd=%p "
           "(async present)",
           s->base.desc.Width, s->base.desc.Height, s->base.desc.Format,
           s->base.desc.BufferCount, (void *)hwnd);

   *out_swapchain = com;
   return NPT_S_OK;
}

#else /* !__WINE__ */

/* Native Win32: the OS DXGI runtime + Triton own presentation; COM-
 * level swapchain creation keeps failing exactly as the host does. */
HRESULT
npt_guest_swapchain12_create(void *queue_unknown,
                             const DXGI_SWAP_CHAIN_DESC1 *desc1,
                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
                             HWND hwnd,
                             void *factory_wrapper,
                             void **out_swapchain)
{
   (void)queue_unknown; (void)desc1; (void)fs_desc; (void)hwnd;
   (void)factory_wrapper;
   if (out_swapchain)
      *out_swapchain = NULL;
   return NPT_E_NOTIMPL;
}

#endif /* __WINE__ */

HRESULT
npt_guest_swapchain12_create_legacy(void *queue_unknown,
                                    const DXGI_SWAP_CHAIN_DESC *desc,
                                    void *factory_wrapper,
                                    void **out_swapchain)
{
   if (!desc)
      return NPT_DXGI_ERROR_INVALID_CALL;

   DXGI_SWAP_CHAIN_DESC1 d1;
   memset(&d1, 0, sizeof(d1));
   d1.Width = desc->BufferDesc.Width;
   d1.Height = desc->BufferDesc.Height;
   d1.Format = desc->BufferDesc.Format;
   d1.SampleDesc = desc->SampleDesc;
   d1.BufferUsage = desc->BufferUsage;
   d1.BufferCount = desc->BufferCount;
   d1.SwapEffect = desc->SwapEffect;
   d1.Flags = desc->Flags;

   DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs;
   memset(&fs, 0, sizeof(fs));
   fs.RefreshRate = desc->BufferDesc.RefreshRate;
   fs.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
   fs.Scaling = desc->BufferDesc.Scaling;
   fs.Windowed = desc->Windowed;

   return npt_guest_swapchain12_create(queue_unknown, &d1, &fs,
                                       desc->OutputWindow, factory_wrapper,
                                       out_swapchain);
}
