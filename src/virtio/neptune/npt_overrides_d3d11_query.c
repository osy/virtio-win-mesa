/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Query / Predicate / Counter feedback slot lifecycle plus the
 * Create* overrides that finalize registration.
 */

#include "npt_com.h"
#include "npt_overrides_d3d11_feedback.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_overrides.h"
#include "npt_ring.h"

#include "neptune-protocol/npt_protocol_client_id3d11asynchronous.h"
#include "neptune-protocol/npt_protocol_client_id3d11counter.h"
#include "neptune-protocol/npt_protocol_client_id3d11device.h"
#include "neptune-protocol/npt_protocol_client_id3d11predicate.h"
#include "neptune-protocol/npt_protocol_client_id3d11query.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* 0 = unknown type; caller falls back to the sync GetData path. */
static uint32_t
npt_query_data_size_for_type(D3D11_QUERY type)
{
   switch (type) {
   case D3D11_QUERY_EVENT:
   case D3D11_QUERY_OCCLUSION_PREDICATE:
   case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
   case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
   case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
   case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
   case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3:
      return (uint32_t)sizeof(BOOL);
   case D3D11_QUERY_OCCLUSION:
   case D3D11_QUERY_TIMESTAMP:
      return (uint32_t)sizeof(UINT64);
   case D3D11_QUERY_TIMESTAMP_DISJOINT:
      return (uint32_t)sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
   case D3D11_QUERY_PIPELINE_STATISTICS:
      return (uint32_t)sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS);
   case D3D11_QUERY_SO_STATISTICS:
   case D3D11_QUERY_SO_STATISTICS_STREAM0:
   case D3D11_QUERY_SO_STATISTICS_STREAM1:
   case D3D11_QUERY_SO_STATISTICS_STREAM2:
   case D3D11_QUERY_SO_STATISTICS_STREAM3:
      return (uint32_t)sizeof(D3D11_QUERY_DATA_SO_STATISTICS);
   default:
      return 0u;
   }
}

/* A create normally goes out async and reports nothing back, so a query
 * type the host cannot make would otherwise fail only at first GetData,
 * tearing the context down.  The first create of each D3D11_QUERY type
 * instead goes out with a reply and the host's HRESULT decides.  One
 * word per type: 0 = unprobed, 1 = the host creates the type (async
 * path from then on), anything else = the latched failing HRESULT,
 * answered guest-side without a round trip.  Racing probes are benign
 * (last write wins, both hold a host answer). */
#define NPT_QUERY_TYPE_SLOTS (D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3 + 1)
static _Atomic int32_t npt_query_create_hr[NPT_QUERY_TYPE_SLOTS];

static bool
npt_query_probe_state(UINT type, int32_t *state)
{
   if (type >= NPT_QUERY_TYPE_SLOTS)
      return false;
   *state = atomic_load_explicit(&npt_query_create_hr[type],
                                 memory_order_acquire);
   return true;
}

/* Transport failures are returned but never latched: a reply mismatch
 * decodes as DEVICE_REMOVED and an encoder that could not acquire
 * returns E_OUTOFMEMORY, neither of which is the host's answer. */
static void
npt_query_probe_latch(UINT type, HRESULT hr)
{
   int32_t state;
   if (NPT_SUCCEEDED(hr))
      state = 1;
   else if (hr != (HRESULT)0x887A0005 /* DXGI_ERROR_DEVICE_REMOVED */ &&
            hr != (HRESULT)0x8007000E /* E_OUTOFMEMORY */)
      state = (int32_t)hr;
   else
      return;
   atomic_store_explicit(&npt_query_create_hr[type], state,
                         memory_order_release);
}

static void query_aux_destroy(void *aux_raw);

static void
query_aux_init(struct npt_com_base *com,
               struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d11_query_aux *aux = com->aux;
   aux->base.com = com;
   aux->base.fb_shmem = NULL;
   aux->base.fb_offset = 0;
   aux->base.registered = false;
   aux->query_data_size = 0;
   atomic_store_explicit(&aux->local_version, 0, memory_order_relaxed);
   atomic_store_explicit(&aux->flushed_version, UINT32_MAX, memory_order_relaxed);
   com->aux_destroy = query_aux_destroy;
   /* aux_init runs before we know D3D11_QUERY_DESC::Query, which
    * determines result size -- finalize_create handles registration. */
   (void)dev; (void)host_id;
}

static void
query_aux_destroy(void *aux_raw)
{
   struct npt_d3d11_query_aux *aux = aux_raw;
   if (aux->base.registered && aux->base.com && aux->base.com->base.device) {
      struct npt_device *dev = aux->base.com->base.device;
      uint32_t seqno = 0;
      if (npt_dispatch_feedback_unregister_query(dev->ring,
                                                 aux->base.com->base.id,
                                                 &seqno) &&
          aux->base.fb_shmem) {
         /* The slot goes back for reuse once the host has run the
          * unregister; the free list takes over the shmem ref. */
         npt_device_free_feedback_slot(dev, aux->base.fb_shmem,
                                       aux->base.fb_offset,
                                       NPT_QUERY_FEEDBACK_SLOT_SIZE, seqno);
         aux->base.fb_shmem = NULL;
      }
   }
   if (aux->base.fb_shmem && aux->base.com && aux->base.com->base.device) {
      npt_renderer_shmem_unref(aux->base.com->base.device->renderer,
                               aux->base.fb_shmem);
   }
   free(aux);
}

struct npt_d3d11_query_aux *
npt_d3d11_query_aux_cast(void *async)
{
   if (!async)
      return NULL;
   /* Identity check: family wrappers carry one of these per-tier vtbl
    * pointers (overrides patch slots in place, never swap the storage),
    * so vtbl equality means "this is a query family wrapper". */
   const void **vt = ((struct npt_com_base *)async)->lpVtbl;
   if (vt != (const void **)&npt_id3d11asynchronous_default_vtbl_storage &&
       vt != (const void **)&npt_id3d11query_default_vtbl_storage &&
       vt != (const void **)&npt_id3d11query1_default_vtbl_storage &&
       vt != (const void **)&npt_id3d11predicate_default_vtbl_storage &&
       vt != (const void **)&npt_id3d11counter_default_vtbl_storage)
      return NULL;
   return ((struct npt_com_base *)async)->aux;
}

static void
npt_d3d11_query_finalize_create(struct npt_device *dev, void *wrapper,
                                D3D11_QUERY type)
{
   if (!dev || !wrapper)
      return;
   struct npt_com_base *com = wrapper;
   struct npt_d3d11_query_aux *aux = com->aux;
   if (!aux)
      return;

   const uint32_t data_size = npt_query_data_size_for_type(type);
   if (!data_size || data_size > NPT_QUERY_FEEDBACK_SLOT_RESULT)
      return;

   /* Idempotent: CreateQuery1 may hit the same wrapper. */
   if (aux->base.registered)
      return;

   uint32_t fb_offset = 0;
   bool fresh = false;
   struct npt_renderer_shmem *shmem =
      npt_device_alloc_feedback_slot(dev, NPT_QUERY_FEEDBACK_SLOT_SIZE,
                                     &fb_offset, &fresh);
   if (!shmem)
      return;

   aux->base.fb_shmem = shmem;
   aux->base.fb_offset = fb_offset;
   aux->query_data_size = data_size;

   /* Fresh shmem requires a roundtrip so REGISTER_QUERY_FEEDBACK's
    * res_id is registered when the host ring thread reads it. */
   if (fresh)
      npt_ring_force_roundtrip(dev->ring);

   if (npt_dispatch_feedback_register_query(dev->ring, com->base.id,
                                            shmem->res_id, fb_offset, data_size))
      aux->base.registered = true;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateQuery_override(void *self, const D3D11_QUERY_DESC *pQueryDesc,
                         ID3D11Query **ppQuery)
{
   int32_t state;
   if (pQueryDesc && ppQuery &&
       npt_query_probe_state(pQueryDesc->Query, &state) && state != 1) {
      if (state != 0) {
         *ppQuery = NULL;
         return (HRESULT)state;
      }
      ID3D11Query *raw = NULL;
      HRESULT hr = npt_call_ID3D11Device_CreateQuery(
         npt_com_self_ring(self), npt_com_self_id(self), pQueryDesc, &raw);
      npt_query_probe_latch(pQueryDesc->Query, hr);
      if (NPT_SUCCEEDED(hr) && raw) {
         *ppQuery = (ID3D11Query *)npt_com_get_or_wrap(
            npt_com_self_device(self), &NPT_IID_ID3D11Query,
            (uint64_t)(uintptr_t)raw, (struct npt_com_base *)self);
         npt_d3d11_query_finalize_create(npt_com_self_device(self), *ppQuery,
                                         pQueryDesc->Query);
      } else {
         *ppQuery = NULL;
      }
      return hr;
   }
   HRESULT hr = npt_id3d11device_default_CreateQuery(self, pQueryDesc, ppQuery);
   if (NPT_SUCCEEDED(hr) && pQueryDesc && ppQuery && *ppQuery) {
      npt_d3d11_query_finalize_create(npt_com_self_device(self), *ppQuery,
                                      pQueryDesc->Query);
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreatePredicate_override(void *self,
                             const D3D11_QUERY_DESC *pPredicateDesc,
                             ID3D11Predicate **ppPredicate)
{
   int32_t state;
   if (pPredicateDesc && ppPredicate &&
       npt_query_probe_state(pPredicateDesc->Query, &state) && state != 1) {
      if (state != 0) {
         *ppPredicate = NULL;
         return (HRESULT)state;
      }
      ID3D11Predicate *raw = NULL;
      HRESULT hr = npt_call_ID3D11Device_CreatePredicate(
         npt_com_self_ring(self), npt_com_self_id(self), pPredicateDesc, &raw);
      npt_query_probe_latch(pPredicateDesc->Query, hr);
      if (NPT_SUCCEEDED(hr) && raw) {
         *ppPredicate = (ID3D11Predicate *)npt_com_get_or_wrap(
            npt_com_self_device(self), &NPT_IID_ID3D11Predicate,
            (uint64_t)(uintptr_t)raw, (struct npt_com_base *)self);
         npt_d3d11_query_finalize_create(npt_com_self_device(self),
                                         *ppPredicate, pPredicateDesc->Query);
      } else {
         *ppPredicate = NULL;
      }
      return hr;
   }
   HRESULT hr = npt_id3d11device_default_CreatePredicate(
      self, pPredicateDesc, ppPredicate);
   if (NPT_SUCCEEDED(hr) && pPredicateDesc && ppPredicate && *ppPredicate) {
      npt_d3d11_query_finalize_create(npt_com_self_device(self),
                                      *ppPredicate, pPredicateDesc->Query);
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateCounter_override(void *self,
                           const D3D11_COUNTER_DESC *pCounterDesc,
                           ID3D11Counter **ppCounter)
{
   HRESULT hr = npt_id3d11device_default_CreateCounter(
      self, pCounterDesc, ppCounter);
   /* No feedback for Counter: not a D3D11_QUERY type, and Linux
    * D3D11 typically stubs Counter creation with E_NOTIMPL anyway. */
   (void)pCounterDesc;
   (void)ppCounter;
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateQuery1_override(void *self, const D3D11_QUERY_DESC1 *pQueryDesc1,
                           ID3D11Query1 **ppQuery1)
{
   /* DESC1::Query is the same D3D11_QUERY enum, so the probe cache is
    * shared with CreateQuery; ContextType doesn't affect host support
    * or feedback sizing. */
   int32_t state;
   if (pQueryDesc1 && ppQuery1 &&
       npt_query_probe_state(pQueryDesc1->Query, &state) && state != 1) {
      if (state != 0) {
         *ppQuery1 = NULL;
         return (HRESULT)state;
      }
      ID3D11Query1 *raw = NULL;
      HRESULT hr = npt_call_ID3D11Device3_CreateQuery1(
         npt_com_self_ring(self), npt_com_self_id(self), pQueryDesc1, &raw);
      npt_query_probe_latch(pQueryDesc1->Query, hr);
      if (NPT_SUCCEEDED(hr) && raw) {
         *ppQuery1 = (ID3D11Query1 *)npt_com_get_or_wrap(
            npt_com_self_device(self), &NPT_IID_ID3D11Query1,
            (uint64_t)(uintptr_t)raw, (struct npt_com_base *)self);
         npt_d3d11_query_finalize_create(npt_com_self_device(self),
                                         *ppQuery1, pQueryDesc1->Query);
      } else {
         *ppQuery1 = NULL;
      }
      return hr;
   }
   HRESULT hr = npt_id3d11device3_default_CreateQuery1(
      self, pQueryDesc1, ppQuery1);
   if (NPT_SUCCEEDED(hr) && pQueryDesc1 && ppQuery1 && *ppQuery1) {
      npt_d3d11_query_finalize_create(npt_com_self_device(self),
                                      *ppQuery1, pQueryDesc1->Query);
   }
   return hr;
}

/* All tiers derive from ID3D11Asynchronous and share one aux. */
static const GUID *const query_tiers[] = {
   &NPT_IID_ID3D11Asynchronous,
   &NPT_IID_ID3D11Query,
   &NPT_IID_ID3D11Query1,
   &NPT_IID_ID3D11Predicate,
   &NPT_IID_ID3D11Counter,
   NULL,
};

#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE5(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device5, m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE4(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device4, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE5(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device3, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE4(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE2(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device2, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device1, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE2(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE1(m, f)

void
npt_overrides_d3d11_query_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(CreateQuery,    dev_CreateQuery_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(CreatePredicate, dev_CreatePredicate_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(CreateCounter,  dev_CreateCounter_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(CreateQuery1,  dev3_CreateQuery1_override);

   npt_com_register_family(query_tiers,
                           sizeof(struct npt_d3d11_query_aux),
                           query_aux_init);
}
