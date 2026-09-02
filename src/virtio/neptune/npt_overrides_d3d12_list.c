/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12GraphicsCommandList{,1..10} and ID3D12CommandAllocator overrides.
 *
 * Close is async.  The generated default is force_sync purely as a
 * cross-ring barrier: a list recorded on thread A's TLS ring may be
 * ExecuteCommandLists'd from thread B's ring, and the host must have
 * decoded the recording (including Close) before it decodes the
 * submission.  A sync Close stalls every recording thread once per
 * list per frame, which serializes exactly the parallel recording
 * D3D12 is built around.  Instead Close encodes async and stamps
 * {ring id, ring seqno} into the list's aux; ExecuteBundle orders the
 * parent's ring after that stamp on the host, and ExecuteCommandLists
 * orders the queue ring after every ring (npt_overrides_d3d12_queue.c).
 *
 * The reverse direction needs the same care.  ExecuteCommandLists
 * rides the queue's ring; a Reset of the list, or of the allocator
 * whose storage the list's commands live in, rides the calling
 * thread's ring, and the host backend reads that storage only when
 * it decodes the submission -- on a deferred backend, long after.
 * Without a barrier the host can reset first and then encode from
 * recycled memory.  So ECL stamps {queue ring id, seqno} on every
 * list it carries, on each list's allocator, and on every bundle the
 * list executes together with the bundle's allocator (the runtime
 * validates a bundle's allocator at the parent's execute, not only at
 * ExecuteBundle).  Reset orders its ring after each stamped ring on
 * the host before its own async dispatch.  Wrapper destruction needs
 * no stamp: the host defers a COM_RELEASE until every ring has
 * decoded past what was published when the release arrived.
 */

#include <stdatomic.h>
#include <stdlib.h>

#include "npt_com.h"
#include "npt_device.h"
#include "npt_overrides.h"
#include "npt_renderer.h"
#include "npt_ring.h"
#include "npt_tls.h"

/* Close reports device removal when its recording could not reach the
 * host; a genuine host-side recording error still surfaces as a decoder
 * fatal (async model). */
#define NPT_DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005L)

#include "neptune-protocol/npt_protocol_client_id3d12commandallocator.h"
#include "neptune-protocol/npt_protocol_client_id3d12device.h"
#include "neptune-protocol/npt_protocol_client_id3d12graphicscommandlist.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_guest_id3d12graphicscommandlist.h"

#define NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist1, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist2, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist3, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist4, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist5, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist6, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist7, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist8, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist9, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist10, m, f)

/* CreateCommandList cascade over every device tier. */
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE_ALL(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device1, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device2, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device3, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device4, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device5, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device6, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device7, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device8, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device9, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device10, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device11, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device12, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device13, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device14, m, f)

static const GUID *const alloc12_tiers[] = {
   &NPT_IID_ID3D12CommandAllocator, NULL,
};

static const GUID *const list12_tiers[] = {
   &NPT_IID_ID3D12GraphicsCommandList,
   &NPT_IID_ID3D12GraphicsCommandList1,
   &NPT_IID_ID3D12GraphicsCommandList2,
   &NPT_IID_ID3D12GraphicsCommandList3,
   &NPT_IID_ID3D12GraphicsCommandList4,
   &NPT_IID_ID3D12GraphicsCommandList5,
   &NPT_IID_ID3D12GraphicsCommandList6,
   &NPT_IID_ID3D12GraphicsCommandList7,
   &NPT_IID_ID3D12GraphicsCommandList8,
   &NPT_IID_ID3D12GraphicsCommandList9,
   &NPT_IID_ID3D12GraphicsCommandList10,
   NULL,
};

/* ---------- execute stamps ---------- */

/*
 * The queue rings an object's submissions rode, each with the seqno
 * its head must reach for the last such submission to be decoded.
 * One slot per ring: DIRECT queues share the DC/SC ring but every
 * compute and copy queue owns one, and an allocator's lists -- or one
 * list executed more than once -- may be submitted to several.  A
 * stamp from a ring the table cannot hold degrades to a full drain.
 *
 * The mutex covers concurrent ExecuteCommandLists on different queues
 * submitting lists recorded from one allocator, which D3D12 permits.
 */
#define NPT_D3D12_EXEC_STAMP_SLOTS 4

struct npt_d3d12_exec_stamp {
   uint64_t ring_id;
   uint32_t seqno;
};

struct npt_d3d12_exec_stamps {
   mtx_t mutex;
   /* Fast path for the common no-submission-pending Reset. */
   _Atomic bool pending;
   bool overflow;
   uint32_t count;
   struct npt_d3d12_exec_stamp slot[NPT_D3D12_EXEC_STAMP_SLOTS];
};

static void
exec_stamps_init(struct npt_d3d12_exec_stamps *s)
{
   mtx_init(&s->mutex, mtx_plain);
   atomic_store_explicit(&s->pending, false, memory_order_relaxed);
   s->overflow = false;
   s->count = 0;
}

static void
exec_stamps_destroy(struct npt_d3d12_exec_stamps *s)
{
   mtx_destroy(&s->mutex);
}

static void
exec_stamps_add(struct npt_d3d12_exec_stamps *s, uint64_t ring_id,
                uint32_t seqno)
{
   mtx_lock(&s->mutex);
   uint32_t i;
   for (i = 0; i < s->count; i++) {
      if (s->slot[i].ring_id == ring_id) {
         /* Seqnos are per ring and wrap; keep the later one. */
         if ((int32_t)(seqno - s->slot[i].seqno) > 0)
            s->slot[i].seqno = seqno;
         break;
      }
   }
   if (i == s->count) {
      if (s->count < NPT_D3D12_EXEC_STAMP_SLOTS) {
         s->slot[s->count].ring_id = ring_id;
         s->slot[s->count].seqno = seqno;
         s->count++;
      } else {
         s->overflow = true;
      }
   }
   atomic_store_explicit(&s->pending, true, memory_order_release);
   mtx_unlock(&s->mutex);
}

/* Order the caller's ring after every stamped submission, then clear.
 * A stamp on the calling thread's own ring is skipped: ring FIFO already
 * orders whatever the caller encodes next behind it. */
static void
exec_stamps_order(struct npt_d3d12_exec_stamps *s, void *self)
{
   if (!atomic_load_explicit(&s->pending, memory_order_acquire))
      return;

   struct npt_d3d12_exec_stamp slot[NPT_D3D12_EXEC_STAMP_SLOTS];
   mtx_lock(&s->mutex);
   const uint32_t count = s->count;
   const bool overflow = s->overflow;
   for (uint32_t i = 0; i < count; i++)
      slot[i] = s->slot[i];
   s->count = 0;
   s->overflow = false;
   atomic_store_explicit(&s->pending, false, memory_order_relaxed);
   mtx_unlock(&s->mutex);

   struct npt_ring *my_ring = npt_com_self_ring(self);
   if (overflow) {
      npt_ring_order_after_all(my_ring);
      return;
   }
   struct npt_ring_edge edges[NPT_D3D12_EXEC_STAMP_SLOTS];
   for (uint32_t i = 0; i < count; i++) {
      edges[i].ring_id = slot[i].ring_id;
      edges[i].seqno = slot[i].seqno;
   }
   npt_ring_order_after_edges(my_ring, edges, count);
}

/* ---------- command allocator ---------- */

/*
 * The host validates an allocator's recording state -- one open list at
 * a time, no Reset while a list records on it -- so CreateCommandList,
 * list Reset and allocator Reset must decode after the Close (or Reset)
 * that last changed that state, which may have gone on another
 * thread's ring.  Each such call records {ring, tail} on the allocator
 * and the next one orders its own ring after it.
 */
struct npt_d3d12_alloc_aux {
   struct npt_d3d12_exec_stamps exec;
   _Atomic uint64_t state_ring_id;
   _Atomic uint32_t state_seqno;
};

static void
alloc12_note_state_change(struct npt_d3d12_alloc_aux *aux,
                          struct npt_ring *ring)
{
   if (!aux || !ring)
      return;
   /* seqno first: a reader that sees the new ring id with the old seqno
    * orders after less than it should; the other order is always
    * safe (a later seqno on the old ring is still before this call). */
   atomic_store_explicit(&aux->state_seqno, npt_ring_seqno_now(ring),
                         memory_order_relaxed);
   atomic_store_explicit(&aux->state_ring_id, ring->id, memory_order_release);
}

static void
alloc12_order_after_state(struct npt_d3d12_alloc_aux *aux,
                          struct npt_ring *ring)
{
   if (!aux || !ring)
      return;
   const uint64_t id =
      atomic_load_explicit(&aux->state_ring_id, memory_order_acquire);
   if (id && id != ring->id)
      npt_ring_order_after(ring, id,
                           atomic_load_explicit(&aux->state_seqno,
                                                memory_order_relaxed));
}

static void npt_d3d12_alloc_aux_destroy(void *aux_raw);

static void
npt_d3d12_alloc_aux_init(struct npt_com_base *com, struct npt_device *dev,
                         uint64_t host_id)
{
   struct npt_d3d12_alloc_aux *aux = com->aux;
   exec_stamps_init(&aux->exec);
   atomic_store_explicit(&aux->state_ring_id, 0, memory_order_relaxed);
   atomic_store_explicit(&aux->state_seqno, 0, memory_order_relaxed);
   com->aux_destroy = npt_d3d12_alloc_aux_destroy;
   (void)dev; (void)host_id;
}

static void
npt_d3d12_alloc_aux_destroy(void *aux_raw)
{
   struct npt_d3d12_alloc_aux *aux = aux_raw;
   exec_stamps_destroy(&aux->exec);
   free(aux);
}

static struct npt_d3d12_alloc_aux *
alloc12_aux(void *self)
{
   return npt_com_family_aux(self, npt_d3d12_alloc_aux_destroy);
}

static HRESULT NPT_STDMETHODCALLTYPE
alloc12_Reset_override(void *self)
{
   struct npt_d3d12_alloc_aux *aux = alloc12_aux(self);
   struct npt_ring *ring = npt_com_self_ring(self);
   if (aux) {
      exec_stamps_order(&aux->exec, self);
      alloc12_order_after_state(aux, ring);
   }
   HRESULT hr = npt_id3d12commandallocator_default_Reset(self);
   alloc12_note_state_change(aux, ring);
   return hr;
}

/* ---------- graphics command list ---------- */

struct npt_d3d12_list_aux {
   /* Stamped by Close, read by the ECL override.  close_pending's
    * release store publishes the two plain fields; Close/ECL are
    * never concurrent on one list (D3D12 contract), and Reset clears
    * close_pending so a stale stamp from a prior recording is never
    * waited on. */
   uint64_t close_ring_id;
   uint32_t close_seqno;
   _Atomic bool close_pending;
   struct npt_d3d12_exec_stamps exec;
   /* The allocator this list records into and the bundles it executes,
    * so an ExecuteCommandLists can stamp them too.  Written while
    * recording (Reset, CreateCommandList, ExecuteBundle); a list
    * records single-threaded, so no synchronisation is needed.  The
    * allocator is a plain pointer: a submission is only valid while it
    * is alive.  Bundles are held by reference, since a parent may be
    * executed after the app has dropped its own. */
   void *allocator;
   void **bundles;
   uint32_t bundle_count;
   uint32_t bundle_cap;
};

static void npt_d3d12_list_aux_destroy(void *aux_raw);

static void
npt_d3d12_list_aux_init(struct npt_com_base *com,
                        struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d12_list_aux *aux = com->aux;
   aux->close_ring_id = 0;
   aux->close_seqno = 0;
   atomic_store_explicit(&aux->close_pending, false, memory_order_relaxed);
   exec_stamps_init(&aux->exec);
   aux->allocator = NULL;
   aux->bundles = NULL;
   aux->bundle_count = 0;
   aux->bundle_cap = 0;
   com->aux_destroy = npt_d3d12_list_aux_destroy;
   /* D3D12 lists record single-threaded but may legally migrate threads
    * between calls; order their wire traffic across rings (npt_object.h
    * ring_ordered / npt_com_self_ring). */
   com->base.ring_ordered = true;
   (void)dev; (void)host_id;
}

static void
list12_drop_bundles(struct npt_d3d12_list_aux *aux)
{
   for (uint32_t i = 0; i < aux->bundle_count; i++)
      npt_com_default_release(aux->bundles[i]);
   aux->bundle_count = 0;
}

static void
npt_d3d12_list_aux_destroy(void *aux_raw)
{
   struct npt_d3d12_list_aux *aux = aux_raw;
   list12_drop_bundles(aux);
   free(aux->bundles);
   exec_stamps_destroy(&aux->exec);
   free(aux);
}

/* NULL on wrappers outside the graphics-command-list family; QI tier
 * aliases (a higher GraphicsCommandList tier) resolve to the primary's
 * aux. */
static struct npt_d3d12_list_aux *
list12_aux(void *self)
{
   return npt_com_family_aux(self, npt_d3d12_list_aux_destroy);
}

/* Retain `bundle` until the next Reset; a bundle executed several
 * times in one recording is held once. */
static void
list12_track_bundle(struct npt_d3d12_list_aux *aux, void *bundle)
{
   for (uint32_t i = 0; i < aux->bundle_count; i++) {
      if (aux->bundles[i] == bundle)
         return;
   }
   if (aux->bundle_count == aux->bundle_cap) {
      const uint32_t cap = aux->bundle_cap ? aux->bundle_cap * 2 : 4;
      void **grown = realloc(aux->bundles, cap * sizeof(*grown));
      if (!grown)
         return;
      aux->bundles = grown;
      aux->bundle_cap = cap;
   }
   npt_com_default_addref(bundle);
   aux->bundles[aux->bundle_count++] = bundle;
}

bool
npt_d3d12_list_close_barrier(void *list_wrapper, uint64_t *out_ring_id,
                             uint32_t *out_seqno)
{
   struct npt_d3d12_list_aux *aux = list12_aux(list_wrapper);
   if (!aux ||
       !atomic_load_explicit(&aux->close_pending, memory_order_acquire))
      return false;
   *out_ring_id = aux->close_ring_id;
   *out_seqno = aux->close_seqno;
   return true;
}

static void
list12_stamp_with_allocator(struct npt_d3d12_list_aux *aux,
                            uint64_t ring_id, uint32_t seqno)
{
   exec_stamps_add(&aux->exec, ring_id, seqno);
   struct npt_d3d12_alloc_aux *alloc =
      aux->allocator ? alloc12_aux(aux->allocator) : NULL;
   if (alloc)
      exec_stamps_add(&alloc->exec, ring_id, seqno);
}

bool
npt_d3d12_list_stamp_execute(void *list_wrapper, uint64_t ring_id,
                             uint32_t seqno)
{
   struct npt_d3d12_list_aux *aux = list12_aux(list_wrapper);
   if (!aux)
      return false;
   list12_stamp_with_allocator(aux, ring_id, seqno);
   /* The submission reads every executed bundle's storage as well. */
   for (uint32_t i = 0; i < aux->bundle_count; i++) {
      struct npt_d3d12_list_aux *baux = list12_aux(aux->bundles[i]);
      if (baux)
         list12_stamp_with_allocator(baux, ring_id, seqno);
   }
   return true;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateCommandList_override(void *self, UINT nodeMask,
                                 D3D12_COMMAND_LIST_TYPE type,
                                 ID3D12CommandAllocator *pCommandAllocator,
                                 ID3D12PipelineState *pInitialState,
                                 const IID *riid, void **ppCommandList)
{
   struct npt_d3d12_alloc_aux *alloc =
      pCommandAllocator ? alloc12_aux(pCommandAllocator) : NULL;
   struct npt_ring *ring = npt_com_self_ring(self);
   alloc12_order_after_state(alloc, ring);
   HRESULT hr = npt_id3d12device_default_CreateCommandList(
      self, nodeMask, type, pCommandAllocator, pInitialState, riid,
      ppCommandList);
   /* The new list records on the allocator from here. */
   alloc12_note_state_change(alloc, ring);
   /* A list created this way starts recording without a Reset, so this is
    * the only place its first allocator is observable.  (CreateCommandList1
    * creates the list closed; its allocator arrives with Reset.) */
   if (NPT_SUCCEEDED(hr) && ppCommandList && *ppCommandList) {
      struct npt_d3d12_list_aux *laux = list12_aux(*ppCommandList);
      if (laux)
         laux->allocator = pCommandAllocator;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
list12_Reset_override(void *self, ID3D12CommandAllocator *pAllocator,
                      ID3D12PipelineState *pInitialState)
{
   struct npt_d3d12_list_aux *aux = list12_aux(self);
   struct npt_d3d12_alloc_aux *alloc =
      pAllocator ? alloc12_aux(pAllocator) : NULL;
   struct npt_ring *ring = npt_com_self_ring(self);
   if (aux) {
      exec_stamps_order(&aux->exec, self);
      atomic_store_explicit(&aux->close_pending, false, memory_order_relaxed);
      aux->allocator = pAllocator;
      list12_drop_bundles(aux);
   }
   alloc12_order_after_state(alloc, ring);
   HRESULT hr = npt_id3d12graphicscommandlist_default_Reset(self, pAllocator,
                                                             pInitialState);
   alloc12_note_state_change(alloc, ring);
   return hr;
}

static void NPT_STDMETHODCALLTYPE
list12_ExecuteBundle_override(void *self, ID3D12GraphicsCommandList *pBundle)
{
   struct npt_d3d12_list_aux *aux = list12_aux(self);
   if (aux && pBundle) {
      /* The host validates that the bundle is closed when it decodes
       * this call, and the bundle's Close may have ridden another
       * thread's ring: the same barrier ExecuteCommandLists applies. */
      uint64_t ring_id;
      uint32_t seqno;
      struct npt_ring *my_ring = npt_com_self_ring(self);
      if (npt_d3d12_list_close_barrier(pBundle, &ring_id, &seqno))
         npt_ring_order_after(my_ring, ring_id, seqno);
      else
         npt_ring_order_after_all(my_ring);
      list12_track_bundle(aux, pBundle);
   }
   npt_id3d12graphicscommandlist_default_ExecuteBundle(self, pBundle);
}

static HRESULT NPT_STDMETHODCALLTYPE
list12_Close_override(void *self)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   struct npt_ring_submit_command submit;
   npt_submit_ID3D12GraphicsCommandList_Close(ring, 0, npt_com_self_id(self),
                                              &submit);

   /* A lost ring dropped the recording on the floor; the app must not
    * ExecuteCommandLists a list the host never saw closed.  MSDN:
    * ID3D12GraphicsCommandList::Close returns the recording error and
    * the list must not be submitted when it fails. */
   if (ring->renderer && npt_renderer_is_lost(ring->renderer))
      return NPT_DXGI_ERROR_DEVICE_REMOVED;

   struct npt_d3d12_list_aux *aux = list12_aux(self);
   if (aux) {
      aux->close_ring_id = ring->id;
      aux->close_seqno = submit.seqno;
      atomic_store_explicit(&aux->close_pending, true, memory_order_release);
      /* The allocator is free for another list once this decodes. */
      alloc12_note_state_change(
         aux->allocator ? alloc12_aux(aux->allocator) : NULL, ring);
   }
   return NPT_S_OK;
}

/* ---------- decode-time CPU descriptor reads ---------- */

/* These read the descriptor when the host records them; order the
 * list's ring after the ring that last wrote each descriptor's heap
 * (npt_d3d12_desc_order_read). */

#define NPT_REGISTER_OVERRIDE_D3D12_LIST4_UP(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist4, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist5, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist6, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist7, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist8, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist9, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12graphicscommandlist10, m, f)

static void NPT_STDMETHODCALLTYPE
list12_OMSetRenderTargets_override(
   void *self, UINT NumRenderTargetDescriptors,
   const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors,
   BOOL RTsSingleHandleToDescriptorRange,
   const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   if (pRenderTargetDescriptors) {
      if (RTsSingleHandleToDescriptorRange) {
         npt_d3d12_desc_order_read(ring, pRenderTargetDescriptors[0].ptr,
                                   NumRenderTargetDescriptors);
      } else {
         for (UINT i = 0; i < NumRenderTargetDescriptors; i++)
            npt_d3d12_desc_order_read(ring, pRenderTargetDescriptors[i].ptr,
                                      1);
      }
   }
   if (pDepthStencilDescriptor)
      npt_d3d12_desc_order_read(ring, pDepthStencilDescriptor->ptr, 1);
   npt_id3d12graphicscommandlist_default_OMSetRenderTargets(
      self, NumRenderTargetDescriptors, pRenderTargetDescriptors,
      RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
}

static void NPT_STDMETHODCALLTYPE
list12_ClearRenderTargetView_override(
   void *self, D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView,
   const FLOAT *ColorRGBA, UINT NumRects, const D3D12_RECT *pRects)
{
   npt_d3d12_desc_order_read(npt_com_self_ring(self), RenderTargetView.ptr, 1);
   npt_id3d12graphicscommandlist_default_ClearRenderTargetView(
      self, RenderTargetView, ColorRGBA, NumRects, pRects);
}

static void NPT_STDMETHODCALLTYPE
list12_ClearDepthStencilView_override(
   void *self, D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
   D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects,
   const D3D12_RECT *pRects)
{
   npt_d3d12_desc_order_read(npt_com_self_ring(self), DepthStencilView.ptr, 1);
   npt_id3d12graphicscommandlist_default_ClearDepthStencilView(
      self, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
}

static void NPT_STDMETHODCALLTYPE
list12_ClearUnorderedAccessViewUint_override(
   void *self, D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
   D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource,
   const UINT *Values, UINT NumRects, const D3D12_RECT *pRects)
{
   npt_d3d12_desc_order_read(npt_com_self_ring(self), ViewCPUHandle.ptr, 1);
   npt_id3d12graphicscommandlist_default_ClearUnorderedAccessViewUint(
      self, ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values,
      NumRects, pRects);
}

static void NPT_STDMETHODCALLTYPE
list12_ClearUnorderedAccessViewFloat_override(
   void *self, D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
   D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource,
   const FLOAT *Values, UINT NumRects, const D3D12_RECT *pRects)
{
   npt_d3d12_desc_order_read(npt_com_self_ring(self), ViewCPUHandle.ptr, 1);
   npt_id3d12graphicscommandlist_default_ClearUnorderedAccessViewFloat(
      self, ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values,
      NumRects, pRects);
}

static void NPT_STDMETHODCALLTYPE
list12_BeginRenderPass_override(
   void *self, UINT NumRenderTargets,
   const D3D12_RENDER_PASS_RENDER_TARGET_DESC *pRenderTargets,
   const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *pDepthStencil,
   D3D12_RENDER_PASS_FLAGS Flags)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   for (UINT i = 0; i < NumRenderTargets && pRenderTargets; i++)
      npt_d3d12_desc_order_read(ring, pRenderTargets[i].cpuDescriptor.ptr, 1);
   if (pDepthStencil)
      npt_d3d12_desc_order_read(ring, pDepthStencil->cpuDescriptor.ptr, 1);
   npt_id3d12graphicscommandlist4_default_BeginRenderPass(
      self, NumRenderTargets, pRenderTargets, pDepthStencil, Flags);
}

void
npt_overrides_d3d12_list_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(OMSetRenderTargets,
                                        list12_OMSetRenderTargets_override);
   NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(ClearRenderTargetView,
                                        list12_ClearRenderTargetView_override);
   NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(ClearDepthStencilView,
                                        list12_ClearDepthStencilView_override);
   NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(
      ClearUnorderedAccessViewUint,
      list12_ClearUnorderedAccessViewUint_override);
   NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(
      ClearUnorderedAccessViewFloat,
      list12_ClearUnorderedAccessViewFloat_override);
   NPT_REGISTER_OVERRIDE_D3D12_LIST4_UP(BeginRenderPass,
                                        list12_BeginRenderPass_override);
   NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(Close, list12_Close_override);
   NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(Reset, list12_Reset_override);
   NPT_REGISTER_OVERRIDE_D3D12_LIST_ALL(ExecuteBundle,
                                        list12_ExecuteBundle_override);
   NPT_REGISTER_OVERRIDE(id3d12commandallocator, Reset,
                         alloc12_Reset_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE_ALL(CreateCommandList,
                                          dev12_CreateCommandList_override);
   npt_com_register_family(list12_tiers,
                           sizeof(struct npt_d3d12_list_aux),
                           npt_d3d12_list_aux_init);
   npt_com_register_family(alloc12_tiers,
                           sizeof(struct npt_d3d12_alloc_aux),
                           npt_d3d12_alloc_aux_init);
}
