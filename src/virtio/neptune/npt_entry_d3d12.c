/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12.dll entry points: forward to the generated host calls and wrap
 * the returned host pointers via the runtime IID -> ctor table.
 */

#include "npt_blob.h"
#include "npt_com.h"
#include "npt_device.h"
#include "npt_env.h"

#include "neptune-protocol/npt_protocol_guest_toplevel.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* d3d12.h value the guest answers locally (never on the wire); the rest
 * of the DXGI HRESULTs come from npt_cs.h. */
#define NPT_DXGI_ERROR_SDK_COMPONENT_MISSING ((HRESULT)0x887A002DL)

/* Serialize in two attempts: first with a stack capacity that covers
 * every real-world root signature; retry with the exact size on
 * E_NOT_SUFFICIENT_BUFFER. */
#define NPT_SERIALIZE_INITIAL_CAPACITY (16u * 1024u)
#define NPT_SERIALIZE_ERROR_CAPACITY   (4u * 1024u)
#define NPT_E_NOT_SUFFICIENT_BUFFER    ((HRESULT)0x8007007AL)

/* Shared factory: the standalone d3d12.dll export and Triton's
 * OpenAdapter12 DDI (tritonDDI12.c) both create the inner Neptune
 * device through here — same pattern as npt_d3d11_create_device_internal.
 * The capset gate lives inside so both entries share it. */
HRESULT
npt_d3d12_create_device_internal(IUnknown *pAdapter,
                                 D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                 REFIID riid,
                                 void **ppDevice);

HRESULT
npt_d3d12_create_device_internal(IUnknown *pAdapter,
                                 D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                 REFIID riid,
                                 void **ppDevice)
{
   npt_com_init();

   /* Ring model, by path.
    *
    * API path (Wine), multi-ring: D3D12's threading contract is that N
    * threads record N command lists with no driver-side serialization,
    * so each thread's recording and device traffic rides its own TLS
    * ring and each queue its own instance ring.  The ordering a single
    * ring gives for free is re-established on the host with ring-to-
    * ring edges (npt_ring_order_after*): ExecuteCommandLists orders the
    * queue ring after every other ring, Reset orders after the
    * submissions that still read its storage, a list that migrates
    * threads orders its new ring after its old one, and the host itself
    * waits for a Create decoded on another ring and defers a release
    * until every ring has passed it.  No guest thread waits on the host
    * for any of it.  Cross-queue order is carried by the queues' own
    * Signal/Wait on the host timeline (Wine: the app's; WDDM: the
    * drain-fence edges in tritonQueue12.c), which never depended on
    * ring FIFO.
    *
    * DDI path (Windows WDDM): single ring by default; the same edges
    * make multi-ring correct there too, and NPT_PERF=multi_ring
    * selects it.  One ring keeps every recording thread's decoder off
    * the host cores the vCPUs need. */
   if (!npt_d3d12_from_ddi)
      npt_env_force_perf(NPT_PERF_MULTI_RING);

   struct npt_device *dev = npt_device_acquire();
   if (!dev)
      return NPT_E_FAIL;

   if (!(dev->renderer->info.caps_flags & NPT_CAPSET_CAP_D3D12)) {
      /* The host advertises no D3D12 backend.  Fail here --
       * deterministically and without wire traffic -- so apps take their
       * own D3D11 fallback path. */
      npt_log("D3D12CreateDevice: host advertises no D3D12 support "
              "(caps=0x%08x)", dev->renderer->info.caps_flags);
      npt_device_release();
      return NPT_DXGI_ERROR_UNSUPPORTED;
   }

   if (!dev->multi_ring_enabled && !npt_d3d12_from_ddi) {
      /* The device singleton latched multi_ring off before this call --
       * the usual cause is the app creating its DXGI factory (or a
       * D3D11 device through the D3D11 DDI), which acquires the device,
       * before D3D12CreateDevice runs.  Upgrading in place is safe
       * because TLS rings and the instance ring are created lazily off
       * this flag; the primary ring keeps its single-ring sizing, which
       * costs memory only. */
      static _Atomic int upgraded;
      if (atomic_exchange_explicit(&upgraded, 1, memory_order_relaxed) == 0)
         npt_log("D3D12CreateDevice: upgrading device to multi-ring "
                 "(device was acquired before D3D12CreateDevice)");
      dev->multi_ring_enabled = true;
   }

   /* NULL ppDevice is a capability query: the API contract says create
    * nothing and report whether creation would succeed.  Forward with a
    * NULL out param so the host answers the same question. */
   void *raw = NULL;
   HRESULT hr = npt_call_D3D12CreateDevice(
      dev->ring, pAdapter, MinimumFeatureLevel, riid,
      ppDevice ? &raw : NULL);

   if (NPT_FAILED(hr) && pAdapter && !raw) {
      /* Host-side adapter LUID matching between the DXGI adapter and
       * the D3D12 backend's own enumeration can fail.  A NULL adapter
       * means "default adapter" -- retry once so a matching failure
       * doesn't take down the whole app. */
      npt_log("D3D12CreateDevice failed (0x%08x) with explicit adapter; "
              "retrying with default adapter", (unsigned)hr);
      hr = npt_call_D3D12CreateDevice(
         dev->ring, NULL, MinimumFeatureLevel, riid,
         ppDevice ? &raw : NULL);
   }

   if (NPT_FAILED(hr) || (ppDevice && !raw)) {
      if (raw)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw);
      npt_device_release();
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   if (!ppDevice) {
      /* Capability query succeeded; nothing was created. */
      npt_device_release();
      return hr;
   }

   void *device_wrapper =
      npt_com_get_or_wrap_or_release(dev, riid,
                                     (uint64_t)(uintptr_t)raw, NULL);
   if (!device_wrapper) {
      npt_device_release();
      return NPT_E_OUTOFMEMORY;
   }
   /* Counter so multiple entry-point calls collapsing onto one
    * wrapper each contribute a balanced acquire/release. */
   atomic_fetch_add_explicit(
      &((struct npt_com_base *)device_wrapper)->base.device_ref_holds,
      1, memory_order_relaxed);

   *ppDevice = device_wrapper;
   return hr;
}

HRESULT NPT_API
D3D12CreateDevice(IUnknown *pAdapter,
                  D3D_FEATURE_LEVEL MinimumFeatureLevel,
                  REFIID riid,
                  void **ppDevice)
{
   return npt_d3d12_create_device_internal(pAdapter, MinimumFeatureLevel,
                                           riid, ppDevice);
}

/* ------------------------------------------------------------------ */
/* Root-signature serialization                                        */
/* ------------------------------------------------------------------ */

static HRESULT
finish_serialize(HRESULT hr,
                 const void *blob_bytes, UINT blob_size,
                 const char *error_bytes, UINT error_size,
                 ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob)
{
   if (ppErrorBlob) {
      *ppErrorBlob = NULL;
      if (error_size) {
         /* Error text is advisory; a failed alloc here must not mask
          * the serializer's own HRESULT. */
         *ppErrorBlob = npt_blob_create(error_bytes, error_size);
      }
   }

   if (NPT_FAILED(hr))
      return hr;

   if (ppBlob) {
      *ppBlob = npt_blob_create(blob_bytes, blob_size);
      if (!*ppBlob)
         return NPT_E_OUTOFMEMORY;
   }
   return hr;
}

/* The generated npt_protocol_directx_types.h declares the two
 * serialize entry points with their WIRE shape (byte-array outputs),
 * so the real-API definitions live under npt_-prefixed names and
 * d3d12.def aliases them to the public export names. */
HRESULT NPT_WINAPI
npt_D3D12SerializeRootSignature(
   const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
   D3D_ROOT_SIGNATURE_VERSION Version,
   ID3DBlob **ppBlob,
   ID3DBlob **ppErrorBlob);

HRESULT NPT_WINAPI
npt_D3D12SerializeRootSignature(
   const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
   D3D_ROOT_SIGNATURE_VERSION Version,
   ID3DBlob **ppBlob,
   ID3DBlob **ppErrorBlob)
{
   if (!pRootSignature)
      return NPT_E_INVALIDARG;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev)
      return NPT_E_FAIL;

   uint8_t stack_blob[NPT_SERIALIZE_INITIAL_CAPACITY];
   char stack_error[NPT_SERIALIZE_ERROR_CAPACITY];
   UINT blob_size = sizeof(stack_blob);
   UINT error_size = sizeof(stack_error);

   HRESULT hr = npt_call_D3D12SerializeRootSignature(
      dev->ring, pRootSignature, Version,
      &blob_size, stack_blob, &error_size, stack_error);

   if (hr == NPT_E_NOT_SUFFICIENT_BUFFER && blob_size > 0) {
      /* Exact-size retry.  blob_size now holds the host's required
       * size; the error window stays stack-sized (truncation ok). */
      void *heap_blob = malloc(blob_size);
      if (!heap_blob) {
         npt_device_release();
         return NPT_E_OUTOFMEMORY;
      }
      error_size = sizeof(stack_error);
      hr = npt_call_D3D12SerializeRootSignature(
         dev->ring, pRootSignature, Version,
         &blob_size, heap_blob, &error_size, stack_error);
      hr = finish_serialize(hr, heap_blob, blob_size,
                            stack_error, error_size, ppBlob, ppErrorBlob);
      free(heap_blob);
      npt_device_release();
      return hr;
   }

   hr = finish_serialize(hr, stack_blob, blob_size,
                         stack_error, error_size, ppBlob, ppErrorBlob);
   npt_device_release();
   return hr;
}

HRESULT NPT_WINAPI
npt_D3D12SerializeVersionedRootSignature(
   const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignature,
   ID3DBlob **ppBlob,
   ID3DBlob **ppErrorBlob);

HRESULT NPT_WINAPI
npt_D3D12SerializeVersionedRootSignature(
   const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignature,
   ID3DBlob **ppBlob,
   ID3DBlob **ppErrorBlob)
{
   if (!pRootSignature)
      return NPT_E_INVALIDARG;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev)
      return NPT_E_FAIL;

   uint8_t stack_blob[NPT_SERIALIZE_INITIAL_CAPACITY];
   char stack_error[NPT_SERIALIZE_ERROR_CAPACITY];
   UINT blob_size = sizeof(stack_blob);
   UINT error_size = sizeof(stack_error);

   HRESULT hr = npt_call_D3D12SerializeVersionedRootSignature(
      dev->ring, pRootSignature,
      &blob_size, stack_blob, &error_size, stack_error);

   if (hr == NPT_E_NOT_SUFFICIENT_BUFFER && blob_size > 0) {
      void *heap_blob = malloc(blob_size);
      if (!heap_blob) {
         npt_device_release();
         return NPT_E_OUTOFMEMORY;
      }
      error_size = sizeof(stack_error);
      hr = npt_call_D3D12SerializeVersionedRootSignature(
         dev->ring, pRootSignature,
         &blob_size, heap_blob, &error_size, stack_error);
      hr = finish_serialize(hr, heap_blob, blob_size,
                            stack_error, error_size, ppBlob, ppErrorBlob);
      free(heap_blob);
      npt_device_release();
      return hr;
   }

   hr = finish_serialize(hr, stack_blob, blob_size,
                         stack_error, error_size, ppBlob, ppErrorBlob);
   npt_device_release();
   return hr;
}

/* ------------------------------------------------------------------ */
/* Guest-local stubs (no wire traffic)                                 */
/* ------------------------------------------------------------------ */

/* Not part of the wire registry, so no generated prototypes exist. */
HRESULT NPT_API
D3D12GetDebugInterface(REFIID riid, void **ppvDebug);
HRESULT NPT_API
D3D12EnableExperimentalFeatures(UINT NumFeatures,
                                const IID *pIIDs,
                                void *pConfigurationStructs,
                                UINT *pConfigurationStructSizes);
HRESULT NPT_API
D3D12GetInterface(REFIID rclsid, REFIID riid, void **ppvDebug);

HRESULT NPT_API
D3D12GetDebugInterface(REFIID riid, void **ppvDebug)
{
   /* The answer a loader gives with no SDK layer installed. */
   (void)riid;
   if (ppvDebug)
      *ppvDebug = NULL;
   return NPT_DXGI_ERROR_SDK_COMPONENT_MISSING;
}

HRESULT NPT_API
D3D12EnableExperimentalFeatures(UINT NumFeatures,
                                const IID *pIIDs,
                                void *pConfigurationStructs,
                                UINT *pConfigurationStructSizes)
{
   (void)pIIDs;
   (void)pConfigurationStructs;
   (void)pConfigurationStructSizes;
   return NumFeatures == 0 ? NPT_S_OK : NPT_E_NOINTERFACE;
}

HRESULT NPT_API
D3D12GetInterface(REFIID rclsid, REFIID riid, void **ppvDebug)
{
   (void)rclsid;
   (void)riid;
   if (ppvDebug)
      *ppvDebug = NULL;
   return NPT_E_NOINTERFACE;
}

HRESULT NPT_API
D3D12CreateRootSignatureDeserializer(const void *pSrcData,
                                     SIZE_T SrcDataSizeInBytes,
                                     REFIID pRootSignatureDeserializerInterface,
                                     void **ppRootSignatureDeserializer)
{
   /* Deserializers hand back descs full of interior pointers; a wire
    * transport for that shape isn't worth it until an app needs one.
    * Documented in docs/NEPTUNE_LIMITATIONS.md. */
   (void)pSrcData;
   (void)SrcDataSizeInBytes;
   (void)pRootSignatureDeserializerInterface;
   if (ppRootSignatureDeserializer)
      *ppRootSignatureDeserializer = NULL;
   return NPT_E_NOTIMPL;
}

HRESULT NPT_API
D3D12CreateVersionedRootSignatureDeserializer(
   const void *pSrcData,
   SIZE_T SrcDataSizeInBytes,
   REFIID pRootSignatureDeserializerInterface,
   void **ppRootSignatureDeserializer)
{
   (void)pSrcData;
   (void)SrcDataSizeInBytes;
   (void)pRootSignatureDeserializerInterface;
   if (ppRootSignatureDeserializer)
      *ppRootSignatureDeserializer = NULL;
   return NPT_E_NOTIMPL;
}
