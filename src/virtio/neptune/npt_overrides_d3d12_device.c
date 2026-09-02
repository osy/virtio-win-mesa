/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12Device{,1..14} overrides:
 *  - CheckFeatureSupport: skip_default because a handful of feature
 *    structs embed guest pointers that the generic in/out byte blob
 *    would smuggle to the host as garbage.  Pointer-free features pass
 *    through the generated call unchanged; FEATURE_LEVELS repacks the
 *    requested-levels array after the struct (host override rebuilds
 *    the pointer); the remaining pointer-bearing features are rejected
 *    until an app needs them.
 *  - CreateCommittedResource{,1,2} / CreateHeap{,1} /
 *    CreatePlacedResource{,1}: CPU-visible buffer heaps are re-routed
 *    onto guest-shmem-backed heaps (npt_d3d12_heap.c) so Map() becomes
 *    a zero-wire persistent pointer.  ANY failure on that path falls
 *    back to the plain generated thunk (phase-1 sync map still works).
 *  - GetDescriptorHandleIncrementSize + the descriptor heap's
 *    GetCPU/GPUDescriptorHandleForHeapStart: immutable host answers
 *    behind sync round-trips; cached after the first call (engines
 *    re-derive descriptor handles from them constantly).
 */

#include "npt_com.h"
#include "npt_d3d12_heap.h"
#include "npt_device.h"
#include "npt_env.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_client_id3d12descriptorheap.h"
#include "neptune-protocol/npt_protocol_client_id3d12device.h"
#include "neptune-protocol/npt_protocol_guest_id3d12device.h"
#include "neptune-protocol/npt_protocol_defs.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE14(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device14, m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE13(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device13, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE14(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE12(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device12, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE13(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE11(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device11, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE12(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE10(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device10, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE11(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE9(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device9, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE10(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE8(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device8, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE9(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE7(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device7, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE8(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE6(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device6, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE7(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE5(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device5, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE6(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE4(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device4, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE5(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE3(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device3, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE4(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE2(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device2, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE3(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device1, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE2(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE1(m, f)

/* ------------------------------------------------------------------ */
/* immutable-getter caches                                             */
/* ------------------------------------------------------------------ */

/* Handle increment sizes are a property of the host implementation
 * (identical for every device the VM can create); 0 = not yet
 * fetched (real increments are never 0 for valid types). */
#define NPT_D3D12_DESCRIPTOR_HEAP_TYPES 4
static _Atomic UINT
   dev12_increment_size_cache[NPT_D3D12_DESCRIPTOR_HEAP_TYPES];

static UINT NPT_STDMETHODCALLTYPE
dev12_GetDescriptorHandleIncrementSize_override(
   void *self, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType)
{
   if ((unsigned)DescriptorHeapType >= NPT_D3D12_DESCRIPTOR_HEAP_TYPES) {
      return npt_id3d12device_default_GetDescriptorHandleIncrementSize(
         self, DescriptorHeapType);
   }
   UINT cached = atomic_load_explicit(
      &dev12_increment_size_cache[DescriptorHeapType],
      memory_order_relaxed);
   if (cached)
      return cached;
   UINT size = npt_id3d12device_default_GetDescriptorHandleIncrementSize(
      self, DescriptorHeapType);
   if (size)
      atomic_store_explicit(
         &dev12_increment_size_cache[DescriptorHeapType], size,
         memory_order_relaxed);
   return size;
}

/* ------------------------------------------------------------------ */
/* descriptor heap ordering                                            */
/* ------------------------------------------------------------------ */

/*
 * A CPU descriptor is written by one device method (Create*View,
 * CreateSampler, CopyDescriptors' destination) and read at DECODE time
 * by another (CopyDescriptors' source, OMSetRenderTargets, Clear*View,
 * BeginRenderPass).  Both are wire commands on the calling thread's
 * ring, so a heap written on thread A and read on thread B has its
 * write and its read on two rings with no order between them, and the
 * host may decode the read first -- a stale view bound, or a copy of a
 * descriptor that is not there yet.  Natively both are plain memory
 * accesses the app already ordered.
 *
 * Per descriptor slot, remember the ring and position of the latest
 * write; a read from another ring orders its ring after that position
 * on the host (npt_ring_order_after, which drops edges already
 * covered).  Per slot, not per heap: threads share staging heaps, and
 * a heap-wide mark would chain every reader to whichever thread wrote
 * the heap last, thousands of times a frame.  Only non-shader-visible
 * heaps take part: they are the only ones a CPU read can name, and a
 * shader-visible heap's contents are consumed by ExecuteCommandLists,
 * which orders after every ring anyway.
 */

struct npt_d3d12_desc_heap {
   uint64_t start;
   uint64_t end;
   uint32_t increment;
   uint32_t count;
   /* Latest write per slot: the ring it went on and that ring's tail
    * after it, packed so a reader takes one atomic load.  Unused when
    * !tracked: a shader-visible heap is in the table only so that
    * lookups of its handles resolve from the per-thread cache instead
    * of taking the table lock on every CopyDescriptors destination. */
   _Atomic uint64_t *slots;
   uint32_t slot_cap;
   bool tracked;
};

/* Ring ids are small monotonic counters (npt_device::next_ring_id), so
 * 32 bits hold them. */
#define DESC_LAST_PACK(ring_id, seqno) \
   (((uint64_t)(ring_id) << 32) | (uint64_t)(seqno))
#define DESC_LAST_RING(last) ((uint32_t)((last) >> 32))
#define DESC_LAST_SEQNO(last) ((uint32_t)(last))

static struct {
   mtx_t mutex;
   _Atomic bool inited;
   /* Sorted by start; disjoint.  Records are allocated individually and
    * never freed -- an unregistered one goes onto `spare` for the next
    * heap -- so a thread's cached pointer always points at a record,
    * whose range check then decides whether it is still the right one. */
   struct npt_d3d12_desc_heap **heaps;
   uint32_t count;
   uint32_t cap;
   struct npt_d3d12_desc_heap **spare;
   uint32_t spare_count;
   uint32_t spare_cap;
   /* Bumped on unregister; invalidates every thread's cache. */
   _Atomic uint32_t gen;
} desc_heaps;

/* Per-thread cache of the last few heaps hit: a thread's descriptor
 * calls alternate between a handful of heaps (its staging heap, the
 * shader-visible heap it copies into, the RTV/DSV heaps it binds), so
 * the lock and the search are only paid on a new heap or after an
 * unregister. */
#define DESC_CACHE_SLOTS 4
static _Thread_local struct {
   struct npt_d3d12_desc_heap *heap[DESC_CACHE_SLOTS];
   uint32_t gen;
   uint32_t next;
} desc_cache;

static void
desc_heaps_init(void)
{
   if (atomic_load_explicit(&desc_heaps.inited, memory_order_acquire))
      return;
   static _Atomic int state;
   int expected = 0;
   if (atomic_compare_exchange_strong(&state, &expected, 1)) {
      mtx_init(&desc_heaps.mutex, mtx_plain);
      atomic_store_explicit(&desc_heaps.inited, true, memory_order_release);
   } else {
      while (!atomic_load_explicit(&desc_heaps.inited, memory_order_acquire))
         thrd_yield();
   }
}

/* Caller holds desc_heaps.mutex.  Index of the heap containing ptr, or
 * -1. */
static int
desc_heaps_find_locked(uint64_t ptr)
{
   uint32_t lo = 0, hi = desc_heaps.count;
   while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      const struct npt_d3d12_desc_heap *h = desc_heaps.heaps[mid];
      if (ptr < h->start)
         hi = mid;
      else if (ptr >= h->end)
         lo = mid + 1;
      else
         return (int)mid;
   }
   return -1;
}

static void
desc_heaps_register(uint64_t start, uint32_t increment, uint32_t count,
                    bool track)
{
   if (!increment || !count)
      return;
   desc_heaps_init();
   mtx_lock(&desc_heaps.mutex);
   struct npt_d3d12_desc_heap *h = NULL;
   const bool spare = desc_heaps.spare_count > 0;
   if (spare)
      h = desc_heaps.spare[--desc_heaps.spare_count];
   else
      h = calloc(1, sizeof(*h));
   if (!h) {
      mtx_unlock(&desc_heaps.mutex);
      return;
   }
   bool ok = true;
   if (track && h->slot_cap < count) {
      _Atomic uint64_t *slots = realloc(h->slots, count * sizeof(*slots));
      if (slots) {
         h->slots = slots;
         h->slot_cap = count;
      } else {
         ok = false;
      }
   }
   if (ok && desc_heaps.count == desc_heaps.cap) {
      const uint32_t cap = desc_heaps.cap ? desc_heaps.cap * 2 : 32;
      struct npt_d3d12_desc_heap **grown =
         realloc(desc_heaps.heaps, cap * sizeof(*grown));
      if (grown) {
         desc_heaps.heaps = grown;
         desc_heaps.cap = cap;
      } else {
         ok = false;
      }
   }
   if (!ok) {
      /* A spare record goes back where it came from (the slot it was
       * popped from is still free); a fresh one is simply dropped. */
      if (spare)
         desc_heaps.spare[desc_heaps.spare_count++] = h;
      else {
         free(h->slots);
         free(h);
      }
      mtx_unlock(&desc_heaps.mutex);
      return;
   }
   if (track)
      memset(h->slots, 0, count * sizeof(h->slots[0]));
   h->tracked = track;
   h->start = start;
   h->end = start + (uint64_t)increment * count;
   h->increment = increment;
   h->count = count;
   uint32_t i = 0;
   while (i < desc_heaps.count && desc_heaps.heaps[i]->start < start)
      i++;
   memmove(&desc_heaps.heaps[i + 1], &desc_heaps.heaps[i],
           (desc_heaps.count - i) * sizeof(desc_heaps.heaps[0]));
   desc_heaps.heaps[i] = h;
   desc_heaps.count++;
   mtx_unlock(&desc_heaps.mutex);
}

static void
desc_heaps_unregister(uint64_t start)
{
   if (!atomic_load_explicit(&desc_heaps.inited, memory_order_acquire))
      return;
   mtx_lock(&desc_heaps.mutex);
   const int i = desc_heaps_find_locked(start);
   struct npt_d3d12_desc_heap *h = i >= 0 ? desc_heaps.heaps[i] : NULL;
   if (h) {
      memmove(&desc_heaps.heaps[i], &desc_heaps.heaps[i + 1],
              (desc_heaps.count - (uint32_t)i - 1) *
                 sizeof(desc_heaps.heaps[0]));
      desc_heaps.count--;
      /* A thread still holding h in its cache sees the new generation
       * and re-resolves; until then h's range keeps answering for the
       * heap that is gone, which can only produce a redundant edge. */
      h->end = h->start;
      atomic_fetch_add_explicit(&desc_heaps.gen, 1, memory_order_release);
      if (desc_heaps.spare_count == desc_heaps.spare_cap) {
         const uint32_t cap = desc_heaps.spare_cap ? desc_heaps.spare_cap * 2
                                                   : 16;
         struct npt_d3d12_desc_heap **grown =
            realloc(desc_heaps.spare, cap * sizeof(*grown));
         if (grown) {
            desc_heaps.spare = grown;
            desc_heaps.spare_cap = cap;
         }
      }
      if (desc_heaps.spare_count < desc_heaps.spare_cap)
         desc_heaps.spare[desc_heaps.spare_count++] = h;
      /* Otherwise the record is simply abandoned; it stays valid memory
       * for any cache that still names it. */
   }
   mtx_unlock(&desc_heaps.mutex);
}

/* The heap containing `handle`, or NULL. */
static struct npt_d3d12_desc_heap *
desc_heaps_lookup(uint64_t handle)
{
   const uint32_t gen =
      atomic_load_explicit(&desc_heaps.gen, memory_order_acquire);
   if (desc_cache.gen == gen) {
      for (uint32_t i = 0; i < DESC_CACHE_SLOTS; i++) {
         struct npt_d3d12_desc_heap *h = desc_cache.heap[i];
         if (h && handle >= h->start && handle < h->end)
            return h;
      }
   } else {
      memset(desc_cache.heap, 0, sizeof(desc_cache.heap));
      desc_cache.gen = gen;
   }

   mtx_lock(&desc_heaps.mutex);
   const int i = desc_heaps_find_locked(handle);
   struct npt_d3d12_desc_heap *h = i >= 0 ? desc_heaps.heaps[i] : NULL;
   const uint32_t gen_now =
      atomic_load_explicit(&desc_heaps.gen, memory_order_relaxed);
   mtx_unlock(&desc_heaps.mutex);
   if (h) {
      if (gen_now != desc_cache.gen) {
         memset(desc_cache.heap, 0, sizeof(desc_cache.heap));
         desc_cache.gen = gen_now;
      }
      desc_cache.heap[desc_cache.next++ % DESC_CACHE_SLOTS] = h;
   }
   return h;
}

/* The slot range [first, first + n) that `handle` and n descriptors
 * after it occupy in h, clipped to the heap; false when handle is
 * outside it. */
static bool
desc_heap_slots(const struct npt_d3d12_desc_heap *h, uint64_t handle,
                uint32_t n, uint32_t *first, uint32_t *count)
{
   if (handle < h->start || handle >= h->end)
      return false;
   *first = (uint32_t)((handle - h->start) / h->increment);
   *count = n < h->count - *first ? n : h->count - *first;
   return true;
}

/* `n` descriptors from `handle` were just written by a command on
 * `ring`. */
static void
npt_d3d12_desc_note_write(struct npt_ring *ring, uint64_t handle, uint32_t n)
{
   if (!ring || !n || !atomic_load_explicit(&desc_heaps.inited,
                                            memory_order_acquire))
      return;
   struct npt_d3d12_desc_heap *h = desc_heaps_lookup(handle);
   uint32_t first, count;
   if (!h || !h->tracked || !desc_heap_slots(h, handle, n, &first, &count))
      return;
   const uint64_t last = DESC_LAST_PACK(ring->id, npt_ring_seqno_now(ring));
   for (uint32_t i = 0; i < count; i++)
      atomic_store_explicit(&h->slots[first + i], last, memory_order_release);
}

void
npt_d3d12_desc_order_read(struct npt_ring *ring, uint64_t handle, uint32_t n)
{
   if (!ring || !n || !atomic_load_explicit(&desc_heaps.inited,
                                            memory_order_acquire))
      return;
   struct npt_d3d12_desc_heap *h = desc_heaps_lookup(handle);
   uint32_t first, count;
   if (!h || !h->tracked || !desc_heap_slots(h, handle, n, &first, &count))
      return;
   for (uint32_t i = 0; i < count; i++) {
      const uint64_t last =
         atomic_load_explicit(&h->slots[first + i], memory_order_acquire);
      const uint64_t ring_id = DESC_LAST_RING(last);
      if (ring_id && ring_id != ring->id)
         npt_ring_order_after(ring, ring_id, DESC_LAST_SEQNO(last));
   }
}

/* Descriptor heap handle starts are immutable per heap; fetched once
 * and answered guest-side afterwards. */
static const GUID *const descriptorheap12_tiers[] = {
   &NPT_IID_ID3D12DescriptorHeap, NULL,
};

struct npt_d3d12_descriptorheap_aux {
   D3D12_CPU_DESCRIPTOR_HANDLE cpu_start;
   D3D12_GPU_DESCRIPTOR_HANDLE gpu_start;
   _Atomic bool cpu_valid;
   _Atomic bool gpu_valid;
   /* From the create desc (dev12_CreateDescriptorHeap_override); the
    * heap joins the ordering table on its first CPU-start fetch, once
    * its range is known. */
   D3D12_DESCRIPTOR_HEAP_TYPE type;
   UINT num_descriptors;
   bool shader_visible;
   bool tracked;
};

static void
npt_d3d12_descriptorheap_aux_destroy(void *aux_raw)
{
   struct npt_d3d12_descriptorheap_aux *aux = aux_raw;
   if (aux->tracked)
      desc_heaps_unregister(aux->cpu_start.ptr);
   free(aux);
}

static void
npt_d3d12_descriptorheap_aux_init(struct npt_com_base *com,
                                  struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d12_descriptorheap_aux *aux = com->aux;
   atomic_store_explicit(&aux->cpu_valid, false, memory_order_relaxed);
   atomic_store_explicit(&aux->gpu_valid, false, memory_order_relaxed);
   aux->type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
   aux->num_descriptors = 0;
   aux->shader_visible = true;
   aux->tracked = false;
   com->aux_destroy = npt_d3d12_descriptorheap_aux_destroy;
   (void)dev; (void)host_id;
}

static struct npt_d3d12_descriptorheap_aux *
descriptorheap12_aux(void *self)
{
   struct npt_com_base *com = self;
   if (!com || com->aux_destroy != npt_d3d12_descriptorheap_aux_destroy)
      return NULL;
   return com->aux;
}

static D3D12_CPU_DESCRIPTOR_HANDLE * NPT_STDMETHODCALLTYPE
descriptorheap12_GetCPUDescriptorHandleForHeapStart_override(
   void *self, D3D12_CPU_DESCRIPTOR_HANDLE *_ret_out)
{
   struct npt_d3d12_descriptorheap_aux *aux = descriptorheap12_aux(self);
   if (!aux) {
      return npt_id3d12descriptorheap_default_GetCPUDescriptorHandleForHeapStart(
         self, _ret_out);
   }
   if (atomic_load_explicit(&aux->cpu_valid, memory_order_acquire)) {
      *_ret_out = aux->cpu_start;
      return _ret_out;
   }
   npt_id3d12descriptorheap_default_GetCPUDescriptorHandleForHeapStart(
      self, _ret_out);
   /* Racing first calls store the same host answer. */
   aux->cpu_start = *_ret_out;
   if (aux->num_descriptors && !aux->tracked &&
       ((struct npt_com_base *)self)->base.parent) {
      const UINT inc = dev12_GetDescriptorHandleIncrementSize_override(
         ((struct npt_com_base *)self)->base.parent, aux->type);
      if (inc) {
         desc_heaps_register(aux->cpu_start.ptr, inc, aux->num_descriptors,
                             !aux->shader_visible);
         aux->tracked = true;
      }
   }
   atomic_store_explicit(&aux->cpu_valid, true, memory_order_release);
   return _ret_out;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateDescriptorHeap_override(void *self,
                                    const D3D12_DESCRIPTOR_HEAP_DESC *pDesc,
                                    const IID *riid, void **ppvHeap)
{
   HRESULT hr = npt_id3d12device_default_CreateDescriptorHeap(
      self, pDesc, riid, ppvHeap);
   if (NPT_SUCCEEDED(hr) && pDesc && ppvHeap && *ppvHeap) {
      struct npt_d3d12_descriptorheap_aux *aux =
         descriptorheap12_aux(*ppvHeap);
      if (aux) {
         aux->type = pDesc->Type;
         aux->num_descriptors = pDesc->NumDescriptors;
         aux->shader_visible =
            (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
      }
   }
   return hr;
}

/* Descriptor writers: forward, then record the write's ring position
 * against the destination heap. */
#define DESC_WRITE_OVERRIDE(iface, method, sig, call, dest)             \
   static void NPT_STDMETHODCALLTYPE                                     \
   iface##_##method##_override sig                                       \
   {                                                                     \
      npt_##iface##_default_##method call;                               \
      npt_d3d12_desc_note_write(npt_com_self_ring(self), (dest).ptr, 1); \
   }

DESC_WRITE_OVERRIDE(id3d12device, CreateShaderResourceView,
   (void *self, ID3D12Resource *pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor),
   (self, pResource, pDesc, DestDescriptor), DestDescriptor)
DESC_WRITE_OVERRIDE(id3d12device, CreateUnorderedAccessView,
   (void *self, ID3D12Resource *pResource, ID3D12Resource *pCounterResource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor),
   (self, pResource, pCounterResource, pDesc, DestDescriptor), DestDescriptor)
DESC_WRITE_OVERRIDE(id3d12device, CreateRenderTargetView,
   (void *self, ID3D12Resource *pResource,
    const D3D12_RENDER_TARGET_VIEW_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor),
   (self, pResource, pDesc, DestDescriptor), DestDescriptor)
DESC_WRITE_OVERRIDE(id3d12device, CreateDepthStencilView,
   (void *self, ID3D12Resource *pResource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor),
   (self, pResource, pDesc, DestDescriptor), DestDescriptor)
DESC_WRITE_OVERRIDE(id3d12device, CreateSampler,
   (void *self, const D3D12_SAMPLER_DESC *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor),
   (self, pDesc, DestDescriptor), DestDescriptor)
DESC_WRITE_OVERRIDE(id3d12device8, CreateSamplerFeedbackUnorderedAccessView,
   (void *self, ID3D12Resource *pTargetedResource,
    ID3D12Resource *pFeedbackResource,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor),
   (self, pTargetedResource, pFeedbackResource, DestDescriptor),
   DestDescriptor)
DESC_WRITE_OVERRIDE(id3d12device11, CreateSampler2,
   (void *self, const D3D12_SAMPLER_DESC2 *pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor),
   (self, pDesc, DestDescriptor), DestDescriptor)

static void NPT_STDMETHODCALLTYPE
dev12_CreateConstantBufferView_override(
   void *self, const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc,
   D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
   npt_id3d12device_default_CreateConstantBufferView(self, pDesc,
                                                     DestDescriptor);
   npt_d3d12_desc_note_write(npt_com_self_ring(self), DestDescriptor.ptr, 1);
}

static void NPT_STDMETHODCALLTYPE
dev12_CopyDescriptors_override(
   void *self, UINT NumDestDescriptorRanges,
   const D3D12_CPU_DESCRIPTOR_HANDLE *pDestDescriptorRangeStarts,
   const UINT *pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
   const D3D12_CPU_DESCRIPTOR_HANDLE *pSrcDescriptorRangeStarts,
   const UINT *pSrcDescriptorRangeSizes,
   D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   /* A NULL size array means every range in it holds one descriptor. */
   for (UINT i = 0; i < NumSrcDescriptorRanges && pSrcDescriptorRangeStarts;
        i++)
      npt_d3d12_desc_order_read(
         ring, pSrcDescriptorRangeStarts[i].ptr,
         pSrcDescriptorRangeSizes ? pSrcDescriptorRangeSizes[i] : 1);
   npt_id3d12device_default_CopyDescriptors(
      self, NumDestDescriptorRanges, pDestDescriptorRangeStarts,
      pDestDescriptorRangeSizes, NumSrcDescriptorRanges,
      pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
      DescriptorHeapsType);
   for (UINT i = 0; i < NumDestDescriptorRanges && pDestDescriptorRangeStarts;
        i++)
      npt_d3d12_desc_note_write(
         ring, pDestDescriptorRangeStarts[i].ptr,
         pDestDescriptorRangeSizes ? pDestDescriptorRangeSizes[i] : 1);
}

static void NPT_STDMETHODCALLTYPE
dev12_CopyDescriptorsSimple_override(
   void *self, UINT NumDescriptors,
   D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
   D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
   D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   npt_d3d12_desc_order_read(ring, SrcDescriptorRangeStart.ptr,
                             NumDescriptors);
   npt_id3d12device_default_CopyDescriptorsSimple(
      self, NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart,
      DescriptorHeapsType);
   npt_d3d12_desc_note_write(ring, DestDescriptorRangeStart.ptr,
                             NumDescriptors);
}

static D3D12_GPU_DESCRIPTOR_HANDLE * NPT_STDMETHODCALLTYPE
descriptorheap12_GetGPUDescriptorHandleForHeapStart_override(
   void *self, D3D12_GPU_DESCRIPTOR_HANDLE *_ret_out)
{
   struct npt_d3d12_descriptorheap_aux *aux = descriptorheap12_aux(self);
   if (!aux) {
      return npt_id3d12descriptorheap_default_GetGPUDescriptorHandleForHeapStart(
         self, _ret_out);
   }
   if (atomic_load_explicit(&aux->gpu_valid, memory_order_acquire)) {
      *_ret_out = aux->gpu_start;
      return _ret_out;
   }
   npt_id3d12descriptorheap_default_GetGPUDescriptorHandleForHeapStart(
      self, _ret_out);
   aux->gpu_start = *_ret_out;
   atomic_store_explicit(&aux->gpu_valid, true, memory_order_release);
   return _ret_out;
}

static HRESULT
dev12_check_feature_levels(void *self,
                           D3D12_FEATURE_DATA_FEATURE_LEVELS *fl)
{
   struct npt_device *dev = npt_com_self_device(self);
   const UINT n = fl->NumFeatureLevels;
   if (n && !fl->pFeatureLevelsRequested)
      return NPT_E_INVALIDARG;

   /* Wire layout: [struct][requested levels array].  The struct's
    * pointer field travels as NULL; the host override rebuilds it from
    * the appended array before calling the host. */
   const size_t total = sizeof(*fl) + (size_t)n * sizeof(D3D_FEATURE_LEVEL);
   uint8_t *tmp = malloc(total);
   if (!tmp)
      return NPT_E_OUTOFMEMORY;

   memcpy(tmp, fl, sizeof(*fl));
   ((D3D12_FEATURE_DATA_FEATURE_LEVELS *)tmp)->pFeatureLevelsRequested = NULL;
   if (n)
      memcpy(tmp + sizeof(*fl), fl->pFeatureLevelsRequested,
             (size_t)n * sizeof(D3D_FEATURE_LEVEL));

   HRESULT hr = npt_call_ID3D12Device_CheckFeatureSupport(
      npt_device_method_ring(dev), npt_com_self_id(self),
      D3D12_FEATURE_FEATURE_LEVELS, tmp, (UINT)total);

   if (NPT_SUCCEEDED(hr))
      fl->MaxSupportedFeatureLevel =
         ((D3D12_FEATURE_DATA_FEATURE_LEVELS *)tmp)->MaxSupportedFeatureLevel;

   free(tmp);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CheckFeatureSupport_override(void *self,
                                   D3D12_FEATURE Feature,
                                   void *pFeatureSupportData,
                                   UINT FeatureSupportDataSize)
{
   if (!pFeatureSupportData || !FeatureSupportDataSize)
      return NPT_E_INVALIDARG;

   switch (Feature) {
   case D3D12_FEATURE_FEATURE_LEVELS:
      if (FeatureSupportDataSize !=
          sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
         return NPT_E_INVALIDARG;
      return dev12_check_feature_levels(self, pFeatureSupportData);

   case D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES:
   case D3D12_FEATURE_QUERY_META_COMMAND:
      /* Both embed guest pointers; no consumer yet.  Reject rather
       * than ship garbage pointers to the host. */
      npt_log("CheckFeatureSupport: unsupported pointer-bearing feature "
              "%u rejected", (unsigned)Feature);
      return NPT_E_INVALIDARG;

   default: {
      /* Pointer-free feature structs are a plain in/out byte blob. */
      struct npt_device *dev = npt_com_self_device(self);
      return npt_call_ID3D12Device_CheckFeatureSupport(
         npt_device_method_ring(dev), npt_com_self_id(self),
         Feature, pFeatureSupportData, FeatureSupportDataSize);
   }
   }
}

/* ==========================================================================
 * Shmem-backed CPU-visible heaps (persistent-map path)
 * ========================================================================== */

/* Heap sizes are 64 KiB multiples (also page-aligned by construction:
 * 64 KiB is a multiple of every guest page size we run on). */
#define NPT_D3D12_HEAP_ALIGN(v) (((uint64_t)(v) + 0xFFFFull) & ~0xFFFFull)

static bool
dev12_iid_equal(const GUID *a, const GUID *b)
{
   return a && b && memcmp(a, b, sizeof(GUID)) == 0;
}

/* CPU-visible per Windows semantics: UPLOAD, READBACK, and CUSTOM
 * with a write-back / write-combine CPU page property. */
static bool
dev12_heap_props_cpu_visible(const D3D12_HEAP_PROPERTIES *p)
{
   if (!p)
      return false;
   switch (p->Type) {
   case D3D12_HEAP_TYPE_UPLOAD:
   case D3D12_HEAP_TYPE_READBACK:
      return true;
   case D3D12_HEAP_TYPE_CUSTOM:
      return p->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
             p->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
   default:
      return false;
   }
}

static bool
dev12_riid_is_resource(const IID *riid)
{
   return dev12_iid_equal(riid, &NPT_IID_ID3D12Resource) ||
          dev12_iid_equal(riid, &NPT_IID_ID3D12Resource1) ||
          dev12_iid_equal(riid, &NPT_IID_ID3D12Resource2);
}

static bool
dev12_committed_shmem_candidate(const D3D12_HEAP_PROPERTIES *props,
                                D3D12_HEAP_FLAGS heap_flags,
                                D3D12_RESOURCE_DIMENSION dimension,
                                uint64_t width,
                                const IID *riid, void **ppv)
{
   return ppv && width > 0 &&
          dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
          dev12_heap_props_cpu_visible(props) &&
          !(heap_flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) &&
          dev12_riid_is_resource(riid);
}

/* Committed CPU-visible buffer -> hidden shmem heap + placed resource
 * at offset 0.  `desc` is D3D12_RESOURCE_DESC or (is_desc1)
 * D3D12_RESOURCE_DESC1; is_desc1 callers are >= ID3D12Device8 so the
 * CreatePlacedResource1 thunk is live host-side. */
static HRESULT
dev12_committed_shmem_create(void *self,
                             const D3D12_HEAP_PROPERTIES *props,
                             D3D12_HEAP_FLAGS heap_flags,
                             const void *desc, bool is_desc1,
                             uint64_t width,
                             D3D12_RESOURCE_STATES initial_state,
                             const D3D12_CLEAR_VALUE *clear,
                             const IID *riid, void **ppv)
{
   const uint64_t size = NPT_D3D12_HEAP_ALIGN(width);

   D3D12_HEAP_DESC app_desc;
   memset(&app_desc, 0, sizeof(app_desc));
   app_desc.SizeInBytes = size;
   app_desc.Properties = *props;
   app_desc.Alignment = 0;
   app_desc.Flags = heap_flags;

   void *heap = npt_d3d12_heap_create_shmem_backed(self, &NPT_IID_ID3D12Heap,
                                                   size, &app_desc);
   if (!heap)
      return NPT_E_FAIL;

   HRESULT hr;
   if (is_desc1) {
      hr = npt_id3d12device8_default_CreatePlacedResource1(
         self, (ID3D12Heap *)heap, 0, (const D3D12_RESOURCE_DESC1 *)desc,
         initial_state, clear, riid, ppv);
   } else {
      hr = npt_id3d12device_default_CreatePlacedResource(
         self, (ID3D12Heap *)heap, 0, (const D3D12_RESOURCE_DESC *)desc,
         initial_state, clear, riid, ppv);
   }

   if (NPT_FAILED(hr) || !*ppv) {
      npt_log("committed_shmem: CreatePlacedResource failed 0x%08x",
              (unsigned)hr);
      npt_com_default_release(heap);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   if (!npt_d3d12_resource_bind_shmem_heap(*ppv, heap, /*heap_offset=*/0,
                                           width, props, heap_flags)) {
      /* No resource aux to carry the fast path -- undo and let the
       * caller fall back to the plain committed create. */
      npt_log("committed_shmem: resource aux bind failed; undoing");
      npt_com_default_release(*ppv);
      *ppv = NULL;
      npt_com_default_release(heap);
      return NPT_E_FAIL;
   }

   /* bind took its own ref on the hidden heap; drop ours. */
   npt_com_default_release(heap);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateCommittedResource_override(
   void *self, const D3D12_HEAP_PROPERTIES *pHeapProperties,
   D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
   D3D12_RESOURCE_STATES InitialResourceState,
   const D3D12_CLEAR_VALUE *pOptimizedClearValue, const IID *riidResource,
   void **ppvResource)
{
   if (pDesc &&
       dev12_committed_shmem_candidate(pHeapProperties, HeapFlags,
                                       pDesc->Dimension, pDesc->Width,
                                       riidResource, ppvResource)) {
      HRESULT hr = dev12_committed_shmem_create(
         self, pHeapProperties, HeapFlags, pDesc, /*is_desc1=*/false,
         pDesc->Width, InitialResourceState, pOptimizedClearValue,
         riidResource, ppvResource);
      if (NPT_SUCCEEDED(hr))
         return hr;
      npt_log("CreateCommittedResource: shmem heap path failed 0x%08x; "
              "using sync-map fallback", (unsigned)hr);
   }
   return npt_id3d12device_default_CreateCommittedResource(
      self, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
      pOptimizedClearValue, riidResource, ppvResource);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateCommittedResource1_override(
   void *self, const D3D12_HEAP_PROPERTIES *pHeapProperties,
   D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
   D3D12_RESOURCE_STATES InitialResourceState,
   const D3D12_CLEAR_VALUE *pOptimizedClearValue,
   ID3D12ProtectedResourceSession *pProtectedSession,
   const IID *riidResource, void **ppvResource)
{
   if (pDesc && !pProtectedSession &&
       dev12_committed_shmem_candidate(pHeapProperties, HeapFlags,
                                       pDesc->Dimension, pDesc->Width,
                                       riidResource, ppvResource)) {
      HRESULT hr = dev12_committed_shmem_create(
         self, pHeapProperties, HeapFlags, pDesc, /*is_desc1=*/false,
         pDesc->Width, InitialResourceState, pOptimizedClearValue,
         riidResource, ppvResource);
      if (NPT_SUCCEEDED(hr))
         return hr;
      npt_log("CreateCommittedResource1: shmem heap path failed 0x%08x; "
              "using sync-map fallback", (unsigned)hr);
   }
   return npt_id3d12device4_default_CreateCommittedResource1(
      self, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
      pOptimizedClearValue, pProtectedSession, riidResource, ppvResource);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateCommittedResource2_override(
   void *self, const D3D12_HEAP_PROPERTIES *pHeapProperties,
   D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc,
   D3D12_RESOURCE_STATES InitialResourceState,
   const D3D12_CLEAR_VALUE *pOptimizedClearValue,
   ID3D12ProtectedResourceSession *pProtectedSession,
   const IID *riidResource, void **ppvResource)
{
   if (pDesc && !pProtectedSession &&
       dev12_committed_shmem_candidate(pHeapProperties, HeapFlags,
                                       pDesc->Dimension, pDesc->Width,
                                       riidResource, ppvResource)) {
      HRESULT hr = dev12_committed_shmem_create(
         self, pHeapProperties, HeapFlags, pDesc, /*is_desc1=*/true,
         pDesc->Width, InitialResourceState, pOptimizedClearValue,
         riidResource, ppvResource);
      if (NPT_SUCCEEDED(hr))
         return hr;
      npt_log("CreateCommittedResource2: shmem heap path failed 0x%08x; "
              "using sync-map fallback", (unsigned)hr);
   }
   return npt_id3d12device8_default_CreateCommittedResource2(
      self, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
      pOptimizedClearValue, pProtectedSession, riidResource, ppvResource);
}

/* Buffer-capable CPU heap without WRITE_WATCH, asked for by a heap
 * IID we can wrap.  DENY_BUFFERS heaps can't take our placed buffers
 * -- textures-only CPU heaps stay on the default path.  Alignment >
 * 64 KiB (MSAA) is also rejected. */
static bool
dev12_heap_shmem_candidate(const D3D12_HEAP_DESC *desc, const IID *riid,
                           void **ppv)
{
   return desc && ppv && desc->SizeInBytes > 0 &&
          dev12_heap_props_cpu_visible(&desc->Properties) &&
          !(desc->Flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) &&
          !(desc->Flags & D3D12_HEAP_FLAG_DENY_BUFFERS) &&
          (desc->Alignment == 0 || desc->Alignment == 0x10000ull) &&
          (dev12_iid_equal(riid, &NPT_IID_ID3D12Heap) ||
           dev12_iid_equal(riid, &NPT_IID_ID3D12Heap1));
}

static void *
dev12_heap_shmem_create(void *self, const D3D12_HEAP_DESC *pDesc,
                        const IID *riid)
{
   D3D12_HEAP_DESC app_desc = *pDesc;
   app_desc.SizeInBytes = NPT_D3D12_HEAP_ALIGN(pDesc->SizeInBytes);
   return npt_d3d12_heap_create_shmem_backed(self, riid,
                                             app_desc.SizeInBytes,
                                             &app_desc);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateHeap_override(void *self, const D3D12_HEAP_DESC *pDesc,
                          const IID *riid, void **ppvHeap)
{
   if (dev12_heap_shmem_candidate(pDesc, riid, ppvHeap)) {
      void *heap = dev12_heap_shmem_create(self, pDesc, riid);
      if (heap) {
         *ppvHeap = heap;
         return NPT_S_OK;
      }
      npt_log("CreateHeap: shmem heap path failed; using host heap");
   }
   return npt_id3d12device_default_CreateHeap(self, pDesc, riid, ppvHeap);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateHeap1_override(void *self, const D3D12_HEAP_DESC *pDesc,
                           ID3D12ProtectedResourceSession *pProtectedSession,
                           const IID *riid, void **ppvHeap)
{
   if (!pProtectedSession && dev12_heap_shmem_candidate(pDesc, riid,
                                                        ppvHeap)) {
      void *heap = dev12_heap_shmem_create(self, pDesc, riid);
      if (heap) {
         *ppvHeap = heap;
         return NPT_S_OK;
      }
      npt_log("CreateHeap1: shmem heap path failed; using host heap");
   }
   return npt_id3d12device4_default_CreateHeap1(self, pDesc,
                                                pProtectedSession, riid,
                                                ppvHeap);
}

/* After a successful placed create on an app-visible shmem heap,
 * record the backing on the resource: pins the heap wrapper for
 * lifetime and (buffers only) enables the zero-wire Map fast path. */
static void
dev12_record_placed_on_shmem_heap(void *heap, uint64_t heap_offset,
                                  uint64_t buffer_width, HRESULT hr,
                                  void **ppv)
{
   if (NPT_FAILED(hr) || !ppv || !*ppv || !heap)
      return;
   struct npt_d3d12_heap_aux *haux = npt_d3d12_heap_aux_cast(heap);
   if (!haux || !haux->shmem_heap)
      return;
   if (!npt_d3d12_resource_bind_shmem_heap(*ppv, heap, heap_offset,
                                           buffer_width,
                                           &haux->app_desc.Properties,
                                           haux->app_desc.Flags)) {
      npt_log("CreatePlacedResource: resource on shmem heap has no aux; "
              "heap lifetime NOT pinned by this resource");
   }
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreatePlacedResource_override(
   void *self, ID3D12Heap *pHeap, UINT64 HeapOffset,
   const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
   const D3D12_CLEAR_VALUE *pOptimizedClearValue, const IID *riid,
   void **ppvResource)
{
   HRESULT hr = npt_id3d12device_default_CreatePlacedResource(
      self, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue,
      riid, ppvResource);
   dev12_record_placed_on_shmem_heap(
      pHeap, HeapOffset,
      (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
         ? pDesc->Width : 0,
      hr, ppvResource);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreatePlacedResource1_override(
   void *self, ID3D12Heap *pHeap, UINT64 HeapOffset,
   const D3D12_RESOURCE_DESC1 *pDesc, D3D12_RESOURCE_STATES InitialState,
   const D3D12_CLEAR_VALUE *pOptimizedClearValue, const IID *riid,
   void **ppvResource)
{
   HRESULT hr = npt_id3d12device8_default_CreatePlacedResource1(
      self, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue,
      riid, ppvResource);
   dev12_record_placed_on_shmem_heap(
      pHeap, HeapOffset,
      (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
         ? pDesc->Width : 0,
      hr, ppvResource);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateCommandQueue_override(void *self,
                                  const D3D12_COMMAND_QUEUE_DESC *pDesc,
                                  const IID *riid, void **ppCommandQueue)
{
   HRESULT hr = npt_id3d12device_default_CreateCommandQueue(
      self, pDesc, riid, ppCommandQueue);
   if (NPT_SUCCEEDED(hr) && pDesc && ppCommandQueue && *ppCommandQueue)
      npt_com_pin_queue_ring(*ppCommandQueue,
                             pDesc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT);
   return hr;
}

void
npt_overrides_d3d12_device_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CheckFeatureSupport,
                                      dev12_CheckFeatureSupport_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateCommandQueue,
                                      dev12_CreateCommandQueue_override);

   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateCommittedResource,
                                      dev12_CreateCommittedResource_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateHeap,
                                      dev12_CreateHeap_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreatePlacedResource,
                                      dev12_CreatePlacedResource_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE4(
      CreateCommittedResource1, dev12_CreateCommittedResource1_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE4(CreateHeap1,
                                       dev12_CreateHeap1_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE8(
      CreateCommittedResource2, dev12_CreateCommittedResource2_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE8(
      CreatePlacedResource1, dev12_CreatePlacedResource1_override);

   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(
      GetDescriptorHandleIncrementSize,
      dev12_GetDescriptorHandleIncrementSize_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateDescriptorHeap,
                                      dev12_CreateDescriptorHeap_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateShaderResourceView,
                                      id3d12device_CreateShaderResourceView_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateUnorderedAccessView,
                                      id3d12device_CreateUnorderedAccessView_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateRenderTargetView,
                                      id3d12device_CreateRenderTargetView_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateDepthStencilView,
                                      id3d12device_CreateDepthStencilView_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateSampler,
                                      id3d12device_CreateSampler_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CreateConstantBufferView,
                                      dev12_CreateConstantBufferView_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CopyDescriptors,
                                      dev12_CopyDescriptors_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE(CopyDescriptorsSimple,
                                      dev12_CopyDescriptorsSimple_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE8(
      CreateSamplerFeedbackUnorderedAccessView,
      id3d12device8_CreateSamplerFeedbackUnorderedAccessView_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE11(CreateSampler2,
                                        id3d12device11_CreateSampler2_override);
   NPT_REGISTER_OVERRIDE(id3d12descriptorheap,
                         GetCPUDescriptorHandleForHeapStart,
                         descriptorheap12_GetCPUDescriptorHandleForHeapStart_override);
   NPT_REGISTER_OVERRIDE(id3d12descriptorheap,
                         GetGPUDescriptorHandleForHeapStart,
                         descriptorheap12_GetGPUDescriptorHandleForHeapStart_override);
   npt_com_register_family(descriptorheap12_tiers,
                           sizeof(struct npt_d3d12_descriptorheap_aux),
                           npt_d3d12_descriptorheap_aux_init);
}
