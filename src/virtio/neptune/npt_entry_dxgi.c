/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXGI.dll entry points: forward to the generated host call and wrap
 * the returned host pointer.
 */

#include "npt_com.h"
#include "npt_device.h"

#include "neptune-protocol/npt_protocol_guest_toplevel.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* Failure paths release both the singleton acquire and the host ref
 * on `raw`, so the use-count stays balanced.  `iid` is the caller's
 * requested interface; `base_iid` is the entry point's own tier, used
 * when the request has no registered ctor (e.g. IID_IUnknown via
 * IID_PPV_ARGS) -- a base-tier wrapper is safe because the caller then
 * reaches it only through IUnknown / QueryInterface. */
static HRESULT
finish_create_dxgi_factory(struct npt_device *dev, const GUID *iid,
                           const GUID *base_iid, void *raw, HRESULT call_hr,
                           void **ppFactory)
{
   if (NPT_FAILED(call_hr) || !raw) {
      if (raw)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw);
      npt_device_release();
      return NPT_FAILED(call_hr) ? call_hr : NPT_E_FAIL;
   }
   const uint64_t host_id = (uint64_t)(uintptr_t)raw;
   void *wrapper = npt_com_get_or_wrap(dev, iid, host_id, NULL);
   if (!wrapper && iid != base_iid)
      wrapper = npt_com_get_or_wrap(dev, base_iid, host_id, NULL);
   if (!wrapper) {
      /* No ctor for either IID: drop the host ref get_or_wrap left held. */
      npt_com_send_release(dev, host_id);
      npt_device_release();
      return NPT_E_OUTOFMEMORY;
   }
   atomic_fetch_add_explicit(
      &((struct npt_com_base *)wrapper)->base.device_ref_holds,
      1, memory_order_relaxed);
   *ppFactory = wrapper;
   return NPT_S_OK;
}

HRESULT NPT_API
CreateDXGIFactory(REFIID riid, void **ppFactory)
{
   if (!ppFactory) return NPT_E_INVALIDARG;
   *ppFactory = NULL;

   npt_com_init();
   /* Top-level: device_ref_holds carries the acquire onto the wrapper. */
   struct npt_device *dev = npt_device_acquire();
   if (!dev) return NPT_E_FAIL;

   void *raw = NULL;
   HRESULT hr = npt_call_CreateDXGIFactory(dev->ring, riid, &raw);
   /* Wrap with the CALLER's riid: the returned pointer is used as that
    * interface directly (e.g. CreateDXGIFactory1(IID_IDXGIFactory2) +
    * CreateSwapChainForHwnd), so a lower-tier vtbl would send calls
    * through slots past the end of the storage. */
   return finish_create_dxgi_factory(dev, riid ? riid : &NPT_IID_IDXGIFactory,
                                     &NPT_IID_IDXGIFactory, raw, hr,
                                     ppFactory);
}

HRESULT NPT_API
CreateDXGIFactory1(REFIID riid, void **ppFactory)
{
   if (!ppFactory) return NPT_E_INVALIDARG;
   *ppFactory = NULL;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev) return NPT_E_FAIL;

   void *raw = NULL;
   HRESULT hr = npt_call_CreateDXGIFactory1(dev->ring, riid, &raw);
   /* Caller's riid; see CreateDXGIFactory. */
   return finish_create_dxgi_factory(dev,
                                     riid ? riid : &NPT_IID_IDXGIFactory1,
                                     &NPT_IID_IDXGIFactory1, raw, hr,
                                     ppFactory);
}

HRESULT NPT_API
CreateDXGIFactory2(UINT flags, REFIID riid, void **ppFactory)
{
   if (!ppFactory) return NPT_E_INVALIDARG;
   *ppFactory = NULL;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev) return NPT_E_FAIL;

   void *raw = NULL;
   HRESULT hr = npt_call_CreateDXGIFactory2(dev->ring, flags, riid, &raw);
   return finish_create_dxgi_factory(dev,
                                     riid ? riid : &NPT_IID_IDXGIFactory2,
                                     &NPT_IID_IDXGIFactory2, raw, hr,
                                     ppFactory);
}
