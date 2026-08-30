/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-side shmem-backed ID3D12Heap allocator + heap-family aux and
 * GetDesc override.  See npt_d3d12_heap.h for the design summary.
 */

#include "npt_d3d12_heap.h"

#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_overrides.h"
#include "npt_ring.h"

#include "neptune-protocol/npt_protocol_client_id3d12heap.h"
#include "neptune-protocol/npt_protocol_defs.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* == CPU-window trimming ===================================================
 * Shmem-backed heaps are the one consumer that can fill the fixed,
 * never-evicted store their windows are carved from (the hostmem BAR
 * under the WDDM2 KMD, shared with the transport): titles hold gigabytes
 * of write-once staging heaps live across a load, far past the store.
 * The CPU window is only needed while a Map is outstanding -- the host
 * GPU reaches the heap through its own mapping -- so heaps whose shmems
 * are window_reclaimable register here and the trimmer returns unmapped
 * windows to the store when an allocation wants room.  Trimming is
 * reactive only: in the common case a window is established once and
 * never moves.  Heaps on permanent-window shmems (wine, vtest, WDDM 1.3
 * locks -- see npt_renderer_shmem) never register and carry none of the
 * bookkeeping.
 *
 * Locking: registry mutex, then per-heap map_mtx by TRYLOCK only -- a
 * held map_mtx means the heap is mid-Map (or mid-restore, which may block
 * on the renderer's submission lock while a transport allocation holding
 * it waits on the trimmer), and skipping it is both correct and the
 * deadlock break. */

static _Atomic int g_heap_trim_state; /* see NPT_CALL_ONCE */
static mtx_t g_heap_trim_mtx;
static struct list_head g_heap_trim_list;

static void
heap_trim_init(void)
{
   mtx_init(&g_heap_trim_mtx, mtx_plain);
   list_inithead(&g_heap_trim_list);
}

/* The tail of the store bulk staging may not consume: kept for the
 * allocations that have no fallback (ring upload growth, map-shadow pool
 * chunks) and for other processes' transport.  Staging has the host-heap
 * fallback, so the cost of the ceiling is a slower create, never a
 * failure. */
#define NPT_SHMEM_BULK_RESERVE (512ull << 20)

static uint64_t
npt_d3d12_heap_trim(struct npt_renderer *renderer, uint64_t want)
{
   uint64_t freed = 0;
   NPT_CALL_ONCE(g_heap_trim_state, heap_trim_init());
   mtx_lock(&g_heap_trim_mtx);
   list_for_each_entry (struct npt_d3d12_shmem_heap, h, &g_heap_trim_list,
                        trim_link) {
      if (h->renderer != renderer)
         continue;
      if (mtx_trylock(&h->map_mtx) != thrd_success)
         continue;
      if (h->active_maps == 0 && h->shmem != NULL &&
          h->shmem->mmap_ptr != NULL &&
          npt_renderer_shmem_window_release(renderer, h->shmem))
         freed += h->size;
      mtx_unlock(&h->map_mtx);
      if (freed >= want)
         break;
   }
   mtx_unlock(&g_heap_trim_mtx);
   if (freed)
      npt_log("d3d12_heap: trimmed %" PRIu64 " bytes of unmapped heap "
              "windows", freed);
   return freed;
}

static void
heap_trim_register(struct npt_d3d12_shmem_heap *h,
                   struct npt_renderer *renderer)
{
   h->renderer = renderer;
   if (!h->shmem->window_reclaimable)
      return;
   NPT_CALL_ONCE(g_heap_trim_state, heap_trim_init());
   mtx_init(&h->map_mtx, mtx_plain);
   h->active_maps = 0;
   mtx_lock(&g_heap_trim_mtx);
   list_addtail(&h->trim_link, &g_heap_trim_list);
   /* Publish the trimmer so transport allocations can retry through it
    * (idempotent: always the same function). */
   renderer->shmem_trim = npt_d3d12_heap_trim;
   mtx_unlock(&g_heap_trim_mtx);
   h->registered = true;
}

static void
heap_trim_unregister(struct npt_d3d12_shmem_heap *h)
{
   if (!h->registered)
      return;
   mtx_lock(&g_heap_trim_mtx);
   list_del(&h->trim_link);
   mtx_unlock(&g_heap_trim_mtx);
   mtx_destroy(&h->map_mtx);
   h->registered = false;
}

void *
npt_d3d12_heap_map_acquire(struct npt_d3d12_shmem_heap *h)
{
   void *ptr = NULL;
   if (h == NULL || h->shmem == NULL)
      return NULL;
   if (!h->registered)
      return h->shmem->mmap_ptr; /* permanent window */
   mtx_lock(&h->map_mtx);
   if (h->shmem->mmap_ptr == NULL &&
       !npt_renderer_shmem_window_restore(h->renderer, h->shmem) &&
       h->renderer->shmem_trim != NULL) {
      /* The store is full of resident windows: free just enough and
       * retry, then everything releasable -- the first pass can free
       * enough bytes without freeing enough CONTIGUOUS bytes.  The
       * trimmer skips this heap (map_mtx is held), and restore may
       * sleep on host round trips, which is fine under map_mtx. */
      if (h->renderer->shmem_trim(h->renderer, h->size) > 0)
         npt_renderer_shmem_window_restore(h->renderer, h->shmem);
      if (h->shmem->mmap_ptr == NULL &&
          h->renderer->shmem_trim(h->renderer, UINT64_MAX) > 0)
         npt_renderer_shmem_window_restore(h->renderer, h->shmem);
   }
   if (h->shmem->mmap_ptr != NULL) {
      h->active_maps++;
      ptr = h->shmem->mmap_ptr;
   }
   mtx_unlock(&h->map_mtx);
   return ptr;
}

void
npt_d3d12_heap_map_release(struct npt_d3d12_shmem_heap *h)
{
   if (h == NULL || !h->registered)
      return;
   mtx_lock(&h->map_mtx);
   assert(h->active_maps > 0);
   if (h->active_maps > 0)
      h->active_maps--;
   mtx_unlock(&h->map_mtx);
}

static const GUID *const heap12_tiers[] = {
   &NPT_IID_ID3D12Heap, &NPT_IID_ID3D12Heap1, NULL,
};

static void npt_d3d12_heap_aux_destroy(void *aux_raw);

/* Needed by aux_destroy, which only receives the aux pointer. */
static void
npt_d3d12_heap_aux_init(struct npt_com_base *com,
                        struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d12_heap_aux *aux = com->aux;
   aux->com = com;
   aux->shmem_heap = NULL;
   aux->has_app_desc = false;
   com->aux_destroy = npt_d3d12_heap_aux_destroy;
   (void)dev; (void)host_id;
}

static void
npt_d3d12_heap_shmem_free(struct npt_renderer *renderer,
                          struct npt_d3d12_shmem_heap *h)
{
   if (!h)
      return;
   heap_trim_unregister(h);
   /* Reaper-deferred blob destroy: COM_RELEASE for the heap was
    * already queued (npt_com_destroy runs before aux_destroy), and the
    * host defers the munmap while the import is live, so out-of-order
    * destruction is safe. */
   if (h->shmem)
      npt_renderer_shmem_unref_async(renderer, h->shmem);
   free(h);
}

static void
npt_d3d12_heap_aux_destroy(void *aux_raw)
{
   struct npt_d3d12_heap_aux *aux = aux_raw;
   if (aux->shmem_heap && aux->com && aux->com->base.device)
      npt_d3d12_heap_shmem_free(aux->com->base.device->renderer,
                                aux->shmem_heap);
   free(aux);
}

struct npt_d3d12_heap_aux *
npt_d3d12_heap_aux_cast(void *heap_wrapper)
{
   return npt_com_family_aux(heap_wrapper, npt_d3d12_heap_aux_destroy);
}

void *
npt_d3d12_heap_create_shmem_backed(void *device_wrapper, const GUID *riid,
                                   uint64_t size,
                                   const D3D12_HEAP_DESC *app_desc)
{
   if (!device_wrapper || !riid || !size || !app_desc)
      return NULL;

   struct npt_device *dev = npt_com_self_device(device_wrapper);
   struct npt_ring *ring = npt_com_self_ring(device_wrapper);
   if (!dev || !dev->renderer || !ring)
      return NULL;

   struct npt_d3d12_shmem_heap *h = calloc(1, sizeof(*h));
   if (!h)
      return NULL;

   /* Bulk ceiling: the store has no eviction, so staging that would eat
    * into the transport reserve first reclaims its own unmapped windows
    * and, when everything left is pinned, degrades to the callers'
    * host-heap fallback instead of starving the ring and pool. */
   uint64_t st_total = 0, st_used = 0;
   if (npt_renderer_shmem_info(dev->renderer, &st_total, &st_used) &&
       st_total > NPT_SHMEM_BULK_RESERVE &&
       st_used + size > st_total - NPT_SHMEM_BULK_RESERVE) {
      npt_d3d12_heap_trim(dev->renderer,
                          st_used + size - (st_total - NPT_SHMEM_BULK_RESERVE));
      if (npt_renderer_shmem_info(dev->renderer, &st_total, &st_used) &&
          st_used + size > st_total - NPT_SHMEM_BULK_RESERVE) {
         npt_log("d3d12_heap: store near capacity (%" PRIu64 "/%" PRIu64
                 "); %" PRIu64 "-byte staging heap degrades to the "
                 "host-heap path", st_used, st_total, size);
         free(h);
         return NULL;
      }
   }

   h->shmem = npt_renderer_shmem_create(dev->renderer, (size_t)size);
   if (!h->shmem) {
      npt_log("d3d12_heap: shmem create (%" PRIu64 " bytes) failed", size);
      free(h);
      return NULL;
   }
   h->size = size;
   heap_trim_register(h, dev->renderer);

   /* The blob was just created on the KMD/context path; the host ring
    * thread may not have observed the resource-table insert yet.
    * Force a virtqueue roundtrip before CREATE_HEAP_FROM_SHMEM names
    * the res_id. */
   npt_ring_force_roundtrip(ring);

   const uint64_t mint_heap_id = npt_com_allocate_next_id();
   HRESULT hr = npt_dispatch_create_heap_from_shmem(
      ring, npt_com_self_id(device_wrapper), mint_heap_id,
      h->shmem->res_id, /*shmem_offset=*/0, size,
      (uint32_t)app_desc->Properties.Type, (uint32_t)app_desc->Flags);
   if (NPT_FAILED(hr)) {
      npt_log("d3d12_heap: CREATE_HEAP_FROM_SHMEM(res=%u size=%" PRIu64
              ") failed 0x%08x", h->shmem->res_id, size, (unsigned)hr);
      npt_d3d12_heap_shmem_free(dev->renderer, h);
      return NULL;
   }

   void *wrapper = npt_com_get_or_wrap_or_release(
      dev, riid, mint_heap_id, (struct npt_com_base *)device_wrapper);
   struct npt_d3d12_heap_aux *aux =
      wrapper ? npt_d3d12_heap_aux_cast(wrapper) : NULL;
   if (!aux) {
      npt_log("d3d12_heap: heap wrapper/aux allocation failed");
      if (wrapper)
         npt_com_default_release(wrapper);
      npt_d3d12_heap_shmem_free(dev->renderer, h);
      return NULL;
   }

   aux->shmem_heap = h;
   aux->app_desc = *app_desc;
   aux->has_app_desc = true;
   return wrapper;
}

/* Shmem-backed heaps answer GetDesc with the APP's original desc; the
 * host heap is an OpenExistingHeapFromAddress import whose desc does
 * not round-trip the app's heap type/flags.  Host-allocated heaps
 * answer with the host's desc, fetched once -- it is immutable per
 * heap and each fetch is a sync wire round-trip.  A failed round trip
 * reports SizeInBytes 0 (zeroed reply); that answer reaches the caller
 * but never enters the cache. */
static D3D12_HEAP_DESC * NPT_STDMETHODCALLTYPE
heap12_GetDesc_override(void *self, D3D12_HEAP_DESC *_ret_out)
{
   struct npt_d3d12_heap_aux *aux = npt_d3d12_heap_aux_cast(self);
   if (!aux)
      return npt_id3d12heap_default_GetDesc(self, _ret_out);
   if (aux->has_app_desc) {
      if (_ret_out)
         *_ret_out = aux->app_desc;
      return _ret_out;
   }
   if (atomic_load_explicit(&aux->host_desc_valid, memory_order_acquire)) {
      if (_ret_out)
         *_ret_out = aux->host_desc;
      return _ret_out;
   }
   D3D12_HEAP_DESC desc;
   memset(&desc, 0, sizeof(desc));
   if (npt_id3d12heap_default_GetDesc(self, &desc) && desc.SizeInBytes) {
      /* Racing first calls store the same host answer; last-write-wins
       * is benign. */
      aux->host_desc = desc;
      atomic_store_explicit(&aux->host_desc_valid, true,
                            memory_order_release);
   }
   if (_ret_out)
      *_ret_out = desc;
   return _ret_out;
}

void
npt_d3d12_heap_overrides_init(void)
{
   NPT_REGISTER_OVERRIDE(id3d12heap, GetDesc, heap12_GetDesc_override);
   NPT_REGISTER_OVERRIDE(id3d12heap1, GetDesc, heap12_GetDesc_override);
   npt_com_register_family(heap12_tiers,
                           sizeof(struct npt_d3d12_heap_aux),
                           npt_d3d12_heap_aux_init);
}
