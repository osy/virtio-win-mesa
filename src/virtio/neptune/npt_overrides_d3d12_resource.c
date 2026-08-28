/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12Resource{,1,2} overrides: Map / Unmap / WriteToSubresource /
 * ReadFromSubresource are skip_default (the generated marshaling would
 * hand the app a garbage host pointer or drop unsized data).
 *
 * Mapping takes one of two paths.
 *
 * Persistent map: resources on shmem-backed heaps (npt_d3d12_heap.c --
 * UPLOAD/READBACK/CPU-CUSTOM buffers) carry {shmem_heap, heap_offset} in
 * their aux, so Map returns shmem_base + heap_offset with no wire
 * traffic at all (the host imported the same pages as the heap's device
 * memory) and Unmap is a local no-op.
 *
 * Sync map: everything else.  Map is a synchronous MAP_RESOURCE round
 * trip -- the host maps the resource and copies its current contents
 * into a per-resource shmem slot, the app writes the slot, and Unmap
 * copies back and unmaps.  Buffers only, and always READ|WRITE so the
 * slot holds the full real contents and a partial app write cannot
 * smear garbage into the bytes it did not touch.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "npt_com.h"
#include "npt_d3d12_heap.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_overrides.h"
#include "npt_resource.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_client_id3d12resource.h"
#include "neptune-protocol/npt_protocol_guest_id3d12resource.h"
#include "neptune-protocol/npt_protocol_defs.h"

#define NPT_REGISTER_OVERRIDE_D3D12_RESOURCE2(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12resource2, m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_RESOURCE1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12resource1, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE2(m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12resource, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE1(m, f)

static const GUID *const resource12_tiers[] = {
   &NPT_IID_ID3D12Resource, &NPT_IID_ID3D12Resource1,
   &NPT_IID_ID3D12Resource2, NULL,
};

struct npt_d3d12_resource_aux {
   struct npt_d3d_map_ring map_ring;

   /* From GetDesc, fetched once on first Map.  dimension < 0 means
    * unqueried.  The texture fields serve WriteToSubresource, which
    * must size its payload from the subresource's own extent when the
    * caller passes no box. */
   uint64_t width;
   int32_t dimension;
   uint32_t height;
   uint32_t depth_or_array;
   uint32_t mip_levels;
   uint32_t format;
   uint32_t layout;

   /* Bytes the sync map path moves for the outstanding map: the buffer
    * width, or a ROW_MAJOR texture's linear footprint.  Map and Unmap
    * MUST agree on it, so it is recorded once at Map time. */
   uint64_t map_bytes;

   /* Texture Map: D3D12 forbids handing out a CPU pointer to a texture
    * whose layout is UNKNOWN (the swizzle is opaque), so the app calls
    * Map(sub, range, NULL) purely to make the resource CPU-accessible
    * and moves pixels with Write/ReadFromSubresource.  That form owns
    * no shmem slot, so Unmap must not run the sync write-back.
    * A ROW_MAJOR texture is NOT this case -- it is linear memory and
    * takes the ordinary pointer path. */
   bool cpu_layout_map;

   /* D3D12 allows nested Map; only the outermost pair round-trips. */
   uint32_t map_count;

   /* Persistent-map path.  shmem_heap is borrowed from hidden_heap's
    * aux; hidden_heap is an owned wrapper ref, kept so the heap -- and
    * with it the host import and guest blob -- outlives this resource.
    * A NULL shmem_heap selects the sync path. */
   struct npt_d3d12_shmem_heap *shmem_heap;
   struct npt_com_base *hidden_heap;
   uint64_t heap_offset;

   /* App's original heap properties, answered guest-side by
    * GetHeapProperties (the host heap is an external-memory import
    * whose properties don't round-trip). */
   bool has_app_heap;
   D3D12_HEAP_PROPERTIES app_heap_props;
   D3D12_HEAP_FLAGS app_heap_flags;

   /* GetGPUVirtualAddress is immutable per resource but a sync wire
    * round-trip; cache the first answer.  valid's release store
    * publishes gpu_va to racing readers. */
   uint64_t gpu_va;
   _Atomic bool gpu_va_valid;

   /* GetDesc likewise: immutable per resource, sync wire round-trip.
    * Cached whole (the scalar fields above cover only what Map and
    * WriteToSubresource need). */
   D3D12_RESOURCE_DESC desc;
   _Atomic bool desc_valid;

   /* Sync-path persistent-map flush list (see npt_d3d12_sync_maps_flush).
    * self is the wrapper this aux belongs to; link/next guarded by
    * g_sync_map_lock. */
   void *self;
   struct npt_d3d12_resource_aux *flush_next;
   bool in_flush_list;
};

/* Where the host cannot import guest shmem as heap memory, every mapped
 * buffer takes the sync path, whose only write-back point is Unmap.  An
 * app that maps its upload buffers once and never unmaps them would
 * then never deliver vertex or constant data to the host, and every
 * frame renders from zeroed buffers.  Track outstanding sync-path maps
 * and flush them to the
 * host before every ExecuteCommandLists: unmap (host copies shadow ->
 * resource) + immediate WRITE-only remap (no READ prime, so a racing
 * app write through the still-valid slot pointer is never overwritten
 * with older host bytes). */
static SRWLOCK g_sync_map_lock = SRWLOCK_INIT;
static struct npt_d3d12_resource_aux *g_sync_map_head;
static _Atomic uint32_t g_sync_map_count;

static void
sync_map_list_add(struct npt_d3d12_resource_aux *aux, void *self)
{
   AcquireSRWLockExclusive(&g_sync_map_lock);
   if (!aux->in_flush_list) {
      aux->self = self;
      aux->flush_next = g_sync_map_head;
      g_sync_map_head = aux;
      aux->in_flush_list = true;
      atomic_fetch_add_explicit(&g_sync_map_count, 1, memory_order_relaxed);
   }
   ReleaseSRWLockExclusive(&g_sync_map_lock);
}

static void
sync_map_list_remove(struct npt_d3d12_resource_aux *aux)
{
   AcquireSRWLockExclusive(&g_sync_map_lock);
   if (aux->in_flush_list) {
      struct npt_d3d12_resource_aux **pp = &g_sync_map_head;
      while (*pp && *pp != aux)
         pp = &(*pp)->flush_next;
      if (*pp)
         *pp = aux->flush_next;
      aux->in_flush_list = false;
      atomic_fetch_sub_explicit(&g_sync_map_count, 1, memory_order_relaxed);
   }
   ReleaseSRWLockExclusive(&g_sync_map_lock);
}

void npt_d3d12_sync_maps_flush(struct npt_ring *queue_ring);

void
npt_d3d12_sync_maps_flush(struct npt_ring *queue_ring)
{
   if (!atomic_load_explicit(&g_sync_map_count, memory_order_relaxed))
      return;
   AcquireSRWLockExclusive(&g_sync_map_lock);
   for (struct npt_d3d12_resource_aux *aux = g_sync_map_head; aux;
        aux = aux->flush_next) {
      if (!aux->map_count || !aux->self)
         continue;
      /* Fire-and-forget on the QUEUE ring: ring FIFO orders the host
       * copy before the ExecuteCommandLists that follows on the same
       * ring, so no reply round trip is needed.  Waiting for one costs
       * hundreds of microseconds per buffer per Execute and dominates
       * submit-thread time.
       *
       * Known race, accepted: an app Unmap on another thread can reach
       * the host through its own ring between these two commands, after
       * which the remap leaves a stale host-side map entry until the
       * resource dies.  That requires unmapping a buffer the app is
       * concurrently submitting from. */
      const uint64_t id = ((struct npt_com_base *)aux->self)->base.id;
      npt_dispatch_resource_unmap12_async(
         queue_ring, id, 0,
         npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0),
         aux->map_bytes,
         npt_d3d_map_ring_slot_offset(&aux->map_ring, 0));
      npt_dispatch_resource_map12_async_write(
         queue_ring, id, 0,
         npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0),
         aux->map_bytes,
         npt_d3d_map_ring_slot_offset(&aux->map_ring, 0));
   }
   ReleaseSRWLockExclusive(&g_sync_map_lock);
}

static void npt_d3d12_resource_aux_destroy(void *aux_raw);

static void
npt_d3d12_resource_aux_init(struct npt_com_base *com,
                            struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d12_resource_aux *aux = com->aux;
   npt_d3d_map_ring_init(&aux->map_ring, com);
   aux->width = 0;
   aux->dimension = -1;
   aux->map_count = 0;
   aux->shmem_heap = NULL;
   aux->hidden_heap = NULL;
   aux->heap_offset = 0;
   aux->has_app_heap = false;
   aux->gpu_va = 0;
   atomic_store_explicit(&aux->gpu_va_valid, false, memory_order_relaxed);
   aux->self = NULL;
   aux->flush_next = NULL;
   aux->in_flush_list = false;
   com->aux_destroy = npt_d3d12_resource_aux_destroy;
   (void)dev; (void)host_id;
}

static void
npt_d3d12_resource_aux_destroy(void *aux_raw)
{
   struct npt_d3d12_resource_aux *aux = aux_raw;
   sync_map_list_remove(aux);
   npt_d3d_map_ring_fini(&aux->map_ring);
   /* COM_RELEASE for the resource was queued by npt_com_destroy before
    * this runs, so the host drops the placed resource before the heap:
    * the heap's release (possibly triggered here) then frees the
    * VkDeviceMemory import in the right order. */
   if (aux->hidden_heap)
      npt_com_default_release(aux->hidden_heap);
   free(aux);
}

/* NULL on wrappers outside the resource family (QI tier aliases
 * resolve to the primary's aux). */
static struct npt_d3d12_resource_aux *
res12_aux(void *self)
{
   return npt_com_family_aux(self, npt_d3d12_resource_aux_destroy);
}

bool
npt_d3d12_resource_bind_shmem_heap(void *resource_wrapper,
                                   void *heap_wrapper,
                                   uint64_t heap_offset,
                                   uint64_t buffer_width,
                                   const D3D12_HEAP_PROPERTIES *app_props,
                                   D3D12_HEAP_FLAGS app_flags)
{
   struct npt_d3d12_resource_aux *aux = res12_aux(resource_wrapper);
   struct npt_d3d12_heap_aux *haux = npt_d3d12_heap_aux_cast(heap_wrapper);
   if (!aux || !haux || !haux->shmem_heap)
      return false;
   if (aux->hidden_heap) {
      npt_log("bind_shmem_heap: resource already bound to a heap");
      return false;
   }

   npt_com_default_addref(heap_wrapper);
   aux->hidden_heap = heap_wrapper;

   if (app_props) {
      aux->has_app_heap = true;
      aux->app_heap_props = *app_props;
      aux->app_heap_flags = app_flags;
   }

   if (buffer_width > 0) {
      if (heap_offset + buffer_width <= haux->shmem_heap->size) {
         aux->shmem_heap = haux->shmem_heap;
         aux->heap_offset = heap_offset;
         aux->width = buffer_width;
         aux->dimension = (int32_t)D3D12_RESOURCE_DIMENSION_BUFFER;
      } else {
         npt_log("bind_shmem_heap: window [%" PRIu64 ", +%" PRIu64
                 ") exceeds heap size %" PRIu64 "; Map stays on the "
                 "sync path", heap_offset, buffer_width,
                 haux->shmem_heap->size);
      }
   }
   return true;
}

/* Cached-desc fetch shared by the GetDesc override and the Map
 * bookkeeping: the desc is immutable per resource but a sync wire
 * round-trip.  A failed round trip reports Dimension UNKNOWN (zeroed
 * reply); that answer reaches the caller but never enters the cache. */
static bool
res12_fetch_desc(void *self, struct npt_d3d12_resource_aux *aux,
                 D3D12_RESOURCE_DESC *out)
{
   if (atomic_load_explicit(&aux->desc_valid, memory_order_acquire)) {
      *out = aux->desc;
      return true;
   }
   memset(out, 0, sizeof(*out));
   if (!npt_id3d12resource_default_GetDesc(self, out) ||
       out->Dimension == D3D12_RESOURCE_DIMENSION_UNKNOWN)
      return false;
   /* Racing first calls store the same host answer; last-write-wins
    * is benign. */
   aux->desc = *out;
   atomic_store_explicit(&aux->desc_valid, true, memory_order_release);
   return true;
}

static D3D12_RESOURCE_DESC * NPT_STDMETHODCALLTYPE
res12_GetDesc_override(void *self, D3D12_RESOURCE_DESC *_ret_out)
{
   struct npt_d3d12_resource_aux *aux = res12_aux(self);
   if (!aux)
      return npt_id3d12resource_default_GetDesc(self, _ret_out);
   D3D12_RESOURCE_DESC desc;
   res12_fetch_desc(self, aux, &desc); /* failure hands the zeroed reply on */
   if (_ret_out)
      *_ret_out = desc;
   return _ret_out;
}

static bool
res12_ensure_desc(void *self, struct npt_d3d12_resource_aux *aux)
{
   if (aux->dimension >= 0)
      return true;
   D3D12_RESOURCE_DESC desc;
   if (!res12_fetch_desc(self, aux, &desc))
      return false;
   aux->width = desc.Width;
   aux->dimension = (int32_t)desc.Dimension;
   aux->height = desc.Height;
   aux->depth_or_array = desc.DepthOrArraySize;
   aux->mip_levels = desc.MipLevels;
   aux->format = (uint32_t)desc.Format;
   aux->layout = (uint32_t)desc.Layout;
   return true;
}

#ifndef D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
#define D3D12_TEXTURE_DATA_PITCH_ALIGNMENT 256
#endif

/* Linear byte footprint of mip 0 of a ROW_MAJOR texture: `rows` rows of
 * a pitch aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, times the depth
 * for a 3D texture.  Returns 0 for a format of unknown size -- the caller
 * then refuses the map rather than guessing, since the host uses this as
 * a straight memcpy bound.
 *
 * The result matches the heap size the runtime reports for the same
 * resource, so a staging texture placed by footprint lands exactly. */
static uint64_t
res12_row_major_bytes(const struct npt_d3d12_resource_aux *aux)
{
   const DXGI_FORMAT fmt = (DXGI_FORMAT)aux->format;
   const uint32_t row_bytes =
      npt_dxgi_format_row_bytes(fmt, (uint32_t)aux->width);
   if (!row_bytes)
      return 0;
   const uint32_t height = aux->height ? aux->height : 1;
   const uint64_t rows = npt_dxgi_format_block_rows(fmt, height);
   const uint64_t align = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
   const uint64_t pitch = ((uint64_t)row_bytes + align - 1) & ~(align - 1);
   uint64_t slices = 1;
   if (aux->dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      slices = aux->depth_or_array ? aux->depth_or_array : 1;
   return pitch * rows * slices;
}

static HRESULT NPT_STDMETHODCALLTYPE
res12_Map_override(void *self, UINT Subresource,
                   const D3D12_RANGE *pReadRange, void **ppData)
{
   (void)pReadRange;  /* Wire always maps READ|WRITE; see file header. */

   struct npt_d3d12_resource_aux *aux = res12_aux(self);
   if (!aux)
      return NPT_E_NOTIMPL;

   /* Persistent-map fast path: the host GPU aliases the shmem pages
    * (VK_EXT_external_memory_host heap import), so the mapping is
    * always live -- no wire traffic, persistent maps just work. */
   if (aux->shmem_heap) {
      if (Subresource != 0)
         return NPT_E_INVALIDARG;
      aux->map_count++;
      if (ppData)
         *ppData = (uint8_t *)npt_d3d12_shmem_heap_ptr(aux->shmem_heap) +
                   aux->heap_offset;
      return NPT_S_OK;
   }

   if (!res12_ensure_desc(self, aux))
      return NPT_E_FAIL;

   uint64_t map_bytes = aux->width;

   if (aux->dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
      /* No-pointer form: the app only wants the resource made CPU-
       * accessible and will move pixels with Write/ReadFromSubresource.
       * Legal for any layout, and owns no shmem slot. */
      if (!ppData) {
         aux->cpu_layout_map = true;
         aux->map_count++;
         return NPT_S_OK;
      }
      /* Pointer form.  D3D12 REQUIRES a texture on a CPU-accessible
       * heap to be ROW_MAJOR, and a ROW_MAJOR texture is plain linear
       * memory the app is entitled to a pointer to, so refusing it
       * fails a legal call -- an app that stages one texture this way
       * during load has no path forward.  Size it from the row-major
       * footprint and let it take the same shmem-slot path as a buffer:
       * the host copies byte_size bytes, having no RowPitch of its own
       * to report on the D3D12 path.
       *
       * Only an opaque (swizzled) layout genuinely has nothing to hand
       * back. */
      if (aux->layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
          !(map_bytes = res12_row_major_bytes(aux))) {
         npt_log("ID3D12Resource::Map: CPU pointer to a non-ROW_MAJOR "
                 "texture is not expressible (dimension %d, fmt %u, "
                 "layout %u, sub %u)",
                 aux->dimension, aux->format, aux->layout, Subresource);
         /* E_OUTOFMEMORY, not E_NOTIMPL: this answer travels back through
          * the runtime's pfnMapHeap, which treats an unexpected failure as
          * fatal and removes the device -- taking the whole app down over a
          * single unmappable texture.  E_OUTOFMEMORY is what the runtime
          * itself returns when a texture cannot be given a CPU address, so
          * the caller sees exactly what a correct driver would report. */
         return NPT_E_OUTOFMEMORY;
      }
   }
   if (Subresource != 0)
      return NPT_E_INVALIDARG;
   if (!map_bytes || map_bytes > 0xffffffffu) {
      npt_log("ID3D12Resource::Map: unsupported map size %" PRIu64
              " (dimension %d, width %" PRIu64 ")",
              map_bytes, aux->dimension, aux->width);
      return NPT_E_FAIL;
   }

   if (aux->map_count > 0) {
      /* Nested map: same pointer, no round-trip. */
      aux->map_count++;
      if (ppData)
         *ppData = npt_d3d_map_ring_slot_ptr(&aux->map_ring, 0);
      return NPT_S_OK;
   }

   aux->map_bytes = map_bytes;
   const uint32_t aligned = ((uint32_t)map_bytes + 63u) & ~63u;
   if (!npt_d3d_map_ring_alloc_shmem(&aux->map_ring, aligned))
      return NPT_E_OUTOFMEMORY;

   struct npt_device *dev = npt_com_self_device(self);
   HRESULT hr = npt_dispatch_resource_map12(
      npt_device_method_ring(dev),
      ((struct npt_com_base *)self)->base.id,
      Subresource,
      NPT_MAP_ACCESS_READ | NPT_MAP_ACCESS_WRITE,
      npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0),
      map_bytes,
      npt_d3d_map_ring_slot_offset(&aux->map_ring, 0),
      /*read_range=*/NPT_MAP_RANGE_NULL, NPT_MAP_RANGE_NULL);
   if (NPT_FAILED(hr))
      return hr;

   aux->map_count = 1;
   sync_map_list_add(aux, self);
   if (ppData)
      *ppData = npt_d3d_map_ring_slot_ptr(&aux->map_ring, 0);
   return NPT_S_OK;
}

static void NPT_STDMETHODCALLTYPE
res12_Unmap_override(void *self, UINT Subresource,
                     const D3D12_RANGE *pWrittenRange)
{
   struct npt_d3d12_resource_aux *aux = res12_aux(self);
   if (!aux || !aux->map_count) {
      npt_log("ID3D12Resource::Unmap: not mapped");
      return;
   }
   if (aux->shmem_heap) {
      /* Persistent-map fast path: local bookkeeping only. */
      aux->map_count--;
      return;
   }
   if (aux->cpu_layout_map) {
      /* Texture CPU-access map: owns no shmem slot, and the pixels
       * already went over the wire in WriteToSubresource. */
      if (--aux->map_count == 0)
         aux->cpu_layout_map = false;
      return;
   }
   if (--aux->map_count > 0)
      return;
   sync_map_list_remove(aux);

   uint64_t written_begin = NPT_MAP_RANGE_NULL;
   uint64_t written_end = NPT_MAP_RANGE_NULL;
   if (pWrittenRange) {
      written_begin = pWrittenRange->Begin;
      written_end = pWrittenRange->End;
   }

   struct npt_device *dev = npt_com_self_device(self);
   HRESULT hr = npt_dispatch_resource_unmap12(
      npt_device_method_ring(dev),
      ((struct npt_com_base *)self)->base.id,
      Subresource,
      npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0),
      aux->map_bytes,
      npt_d3d_map_ring_slot_offset(&aux->map_ring, 0),
      written_begin, written_end);
   if (NPT_FAILED(hr))
      npt_log("ID3D12Resource::Unmap: host unmap failed 0x%08x",
              (unsigned)hr);
}

static D3D12_GPU_VIRTUAL_ADDRESS NPT_STDMETHODCALLTYPE
res12_GetGPUVirtualAddress_override(void *self)
{
   struct npt_d3d12_resource_aux *aux = res12_aux(self);
   if (!aux)
      return npt_id3d12resource_default_GetGPUVirtualAddress(self);
   if (atomic_load_explicit(&aux->gpu_va_valid, memory_order_acquire))
      return aux->gpu_va;
   D3D12_GPU_VIRTUAL_ADDRESS va =
      npt_id3d12resource_default_GetGPUVirtualAddress(self);
   /* Racing first calls store the same host answer; last-write-wins
    * is benign. */
   aux->gpu_va = va;
   atomic_store_explicit(&aux->gpu_va_valid, true, memory_order_release);
   return va;
}

static HRESULT NPT_STDMETHODCALLTYPE
res12_GetHeapProperties_override(void *self,
                                 D3D12_HEAP_PROPERTIES *pHeapProperties,
                                 D3D12_HEAP_FLAGS *pHeapFlags)
{
   struct npt_d3d12_resource_aux *aux = res12_aux(self);
   if (aux && aux->has_app_heap) {
      if (pHeapProperties)
         *pHeapProperties = aux->app_heap_props;
      if (pHeapFlags)
         *pHeapFlags = aux->app_heap_flags;
      return NPT_S_OK;
   }
   return npt_id3d12resource_default_GetHeapProperties(self, pHeapProperties,
                                                       pHeapFlags);
}

/* Rows and slices the payload must carry for this write.  `rows` counts
 * block-rows for compressed formats, matching how the pitches are
 * defined. */
static bool
res12_write_extent(struct npt_d3d12_resource_aux *aux, UINT Subresource,
                   const D3D12_BOX *pDstBox, uint32_t *out_rows,
                   uint32_t *out_slices, uint32_t *out_row_bytes)
{
   const DXGI_FORMAT fmt = (DXGI_FORMAT)aux->format;
   uint32_t w, h, d;

   if (pDstBox) {
      if (pDstBox->right <= pDstBox->left || pDstBox->bottom <= pDstBox->top ||
          pDstBox->back <= pDstBox->front)
         return false;   /* empty box: nothing to send */
      w = pDstBox->right - pDstBox->left;
      h = pDstBox->bottom - pDstBox->top;
      d = pDstBox->back - pDstBox->front;
   } else {
      /* Whole subresource: derive the mip's extent.  Array slices share
       * the mip chain, so the mip index is the subresource modulo the
       * mip count. */
      const uint32_t mips = aux->mip_levels ? aux->mip_levels : 1u;
      const uint32_t mip = Subresource % mips;
      w = (uint32_t)(aux->width >> mip);
      h = aux->height >> mip;
      d = (aux->dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
             ? (aux->depth_or_array >> mip) : 1u;
      if (!w) w = 1;
      if (!h) h = 1;
      if (!d) d = 1;
   }

   /* out_row_bytes is 0 for a format of unknown size; the caller then
    * sizes the payload from the app's own pitch. */
   *out_slices = d;
   *out_row_bytes = npt_dxgi_format_row_bytes(fmt, w);
   *out_rows = *out_row_bytes ? npt_dxgi_format_block_rows(fmt, h) : h;
   return true;
}

/*
 * Textures on CPU-accessible heaps move their pixels through this call
 * rather than a mapped pointer (see the Map override).  The wire's
 * RESOURCE_UPDATE transport already carries exactly this shape -- box,
 * both pitches, payload trailer -- and its D3D12 dispatcher calls
 * ID3D12Resource::WriteToSubresource on the host resource, so the whole
 * job here is sizing the payload and handing it over.
 */
static HRESULT NPT_STDMETHODCALLTYPE
res12_WriteToSubresource_override(void *self, UINT DstSubresource,
                                  const D3D12_BOX *pDstBox,
                                  const void *pSrcData,
                                  UINT SrcRowPitch, UINT SrcDepthPitch)
{
   struct npt_d3d12_resource_aux *aux = res12_aux(self);
   if (!aux || !pSrcData)
      return NPT_E_INVALIDARG;
   if (!res12_ensure_desc(self, aux))
      return NPT_E_FAIL;
   if (aux->dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return NPT_E_INVALIDARG;

   uint32_t rows = 0, slices = 0, row_bytes = 0;
   if (!res12_write_extent(aux, DstSubresource, pDstBox, &rows, &slices,
                           &row_bytes))
      return NPT_S_OK;   /* empty box */

   if (!SrcRowPitch)
      SrcRowPitch = row_bytes;
   if (!SrcRowPitch)
      return NPT_E_INVALIDARG;
   if (!SrcDepthPitch)
      SrcDepthPitch = SrcRowPitch * rows;

   /* Rectangular extent the host may consume.  The host reads only
    * row_bytes out of each row, so the final row's pitch padding never
    * matters -- but reserving it keeps the host's pitch arithmetic
    * valid.  `copy` trims that trailing padding off the read of the
    * app's buffer, which is the one part of the rectangle the app is
    * not obliged to have allocated. */
   const uint64_t full = (uint64_t)SrcDepthPitch * (slices - 1u) +
                         (uint64_t)SrcRowPitch * rows;
   uint64_t copy = full;
   if (row_bytes && SrcRowPitch > row_bytes)
      copy = full - (SrcRowPitch - row_bytes);

   if (!full || full > (uint64_t)(64u << 20)) {
      npt_log("ID3D12Resource::WriteToSubresource: implausible payload "
              "%" PRIu64 " bytes (sub=%u rows=%u slices=%u pitch=%u)",
              full, DstSubresource, rows, slices, SrcRowPitch);
      return NPT_E_INVALIDARG;
   }

   D3D11_BOX box;
   const D3D11_BOX *pbox = NULL;
   if (pDstBox) {
      box.left   = pDstBox->left;
      box.top    = pDstBox->top;
      box.front  = pDstBox->front;
      box.right  = pDstBox->right;
      box.bottom = pDstBox->bottom;
      box.back   = pDstBox->back;
      pbox = &box;
   }

   struct npt_device *dev = npt_com_self_device(self);
   if (!npt_dispatch_resource_update(npt_device_method_ring(dev),
                                     ((struct npt_com_base *)self)->base.id,
                                     DstSubresource, SrcRowPitch,
                                     SrcDepthPitch, pbox, pSrcData,
                                     (uint32_t)full, (uint32_t)copy))
      return NPT_E_FAIL;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
res12_ReadFromSubresource_override(void *self, void *pDstData,
                                   UINT DstRowPitch, UINT DstDepthPitch,
                                   UINT SrcSubresource,
                                   const D3D12_BOX *pSrcBox)
{
   (void)self;
   (void)pDstData;
   (void)DstRowPitch;
   (void)DstDepthPitch;
   (void)SrcSubresource;
   (void)pSrcBox;
   npt_log("ID3D12Resource::ReadFromSubresource not implemented "
           "(CUSTOM-heap textures; see NEPTUNE_LIMITATIONS.md)");
   return NPT_E_NOTIMPL;
}

void
npt_overrides_d3d12_resource_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(Map, res12_Map_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(Unmap, res12_Unmap_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(GetDesc, res12_GetDesc_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(GetGPUVirtualAddress,
                                        res12_GetGPUVirtualAddress_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(GetHeapProperties,
                                        res12_GetHeapProperties_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(WriteToSubresource,
                                        res12_WriteToSubresource_override);
   NPT_REGISTER_OVERRIDE_D3D12_RESOURCE(ReadFromSubresource,
                                        res12_ReadFromSubresource_override);
   npt_com_register_family(resource12_tiers,
                           sizeof(struct npt_d3d12_resource_aux),
                           npt_d3d12_resource_aux_init);
}
