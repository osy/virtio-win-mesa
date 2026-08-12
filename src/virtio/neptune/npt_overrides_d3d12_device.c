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
};

static void
npt_d3d12_descriptorheap_aux_destroy(void *aux_raw)
{
   free(aux_raw);
}

static void
npt_d3d12_descriptorheap_aux_init(struct npt_com_base *com,
                                  struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d12_descriptorheap_aux *aux = com->aux;
   atomic_store_explicit(&aux->cpu_valid, false, memory_order_relaxed);
   atomic_store_explicit(&aux->gpu_valid, false, memory_order_relaxed);
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
   atomic_store_explicit(&aux->cpu_valid, true, memory_order_release);
   return _ret_out;
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
