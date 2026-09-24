/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Carry the originating resource's concrete IID across each
 * Create*View round-trip so View::GetResource can wrap the returned
 * pointer with the right derived vtbl.
 *
 * Apps routinely cast View::GetResource's output straight to
 * Texture2D / Buffer / ... without QI; on Microsoft's d3d11.dll this
 * works because the object's actual vtbl is the most-derived type's.
 * Our generated ID3D11Resource wrapper only exposes the 10 base
 * methods, so a Texture2D-slot call would jump through a NULL slot.
 * Stash the concrete IID at Create*View time and use it in the
 * GetResource override.
 */

#include "npt_com.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_client_id3d11buffer.h"
#include "neptune-protocol/npt_protocol_client_id3d11depthstencilview.h"
#include "neptune-protocol/npt_protocol_client_id3d11device.h"
#include "neptune-protocol/npt_protocol_client_id3d11rendertargetview.h"
#include "neptune-protocol/npt_protocol_client_id3d11resource.h"
#include "neptune-protocol/npt_protocol_client_id3d11shaderresourceview.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture1d.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture2d.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture3d.h"
#include "neptune-protocol/npt_protocol_client_id3d11unorderedaccessview.h"
#include "neptune-protocol/npt_protocol_client_id3d11view.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_guest_id3d11view.h"

#include "npt_device.h"


struct npt_d3d11_view_aux {
   /* IID of the resource this view was created from.  Used by
    * GetResource to wrap the returned pointer with the matching
    * derived vtbl (Buffer / Texture1D / Texture2D / Texture3D)
    * instead of the base ID3D11Resource vtbl.  NULL falls back to
    * IID_ID3D11Resource (e.g. views constructed outside our
    * Create*View overrides). */
   const GUID *resource_iid;
};


/* Map a resource wrapper's lpVtbl back to its concrete IID. */
static const GUID *
view_iid_for_resource_wrapper(void *resource)
{
   if (!resource)
      return &NPT_IID_ID3D11Resource;
   const void **vt = ((struct npt_com_base *)resource)->lpVtbl;
   if (vt == (const void **)&npt_id3d11buffer_default_vtbl_storage)
      return &NPT_IID_ID3D11Buffer;
   if (vt == (const void **)&npt_id3d11texture1d_default_vtbl_storage)
      return &NPT_IID_ID3D11Texture1D;
   if (vt == (const void **)&npt_id3d11texture2d_default_vtbl_storage)
      return &NPT_IID_ID3D11Texture2D;
   if (vt == (const void **)&npt_id3d11texture2d1_default_vtbl_storage)
      return &NPT_IID_ID3D11Texture2D1;
   if (vt == (const void **)&npt_id3d11texture3d_default_vtbl_storage)
      return &NPT_IID_ID3D11Texture3D;
   if (vt == (const void **)&npt_id3d11texture3d1_default_vtbl_storage)
      return &NPT_IID_ID3D11Texture3D1;
   return &NPT_IID_ID3D11Resource;
}


static void
view_record_resource_iid(void *view, void *pResource)
{
   if (!view)
      return;
   struct npt_d3d11_view_aux *aux = ((struct npt_com_base *)view)->aux;
   if (!aux)
      return;
   aux->resource_iid = view_iid_for_resource_wrapper(pResource);
}


/* Per-Device Create*View overrides: call the generated default,
 * then stamp the new view's aux with the originating resource's
 * IID.  All paths fall through to the default if the view-creation
 * failed or the out-pointer is NULL. */

#define VIEW_CREATE_OVERRIDE(LOWER, NAME, VIEW_T, DESC_T)             \
static HRESULT NPT_STDMETHODCALLTYPE                                  \
dev_##NAME##_override(void *self, ID3D11Resource *pResource,          \
                      const DESC_T *pDesc, VIEW_T **ppView)           \
{                                                                     \
   HRESULT hr = npt_##LOWER##_default_##NAME(                         \
      self, pResource, pDesc, ppView);                                \
   if (NPT_SUCCEEDED(hr) && ppView && *ppView)                        \
      view_record_resource_iid(*ppView, pResource);                   \
   return hr;                                                         \
}

VIEW_CREATE_OVERRIDE(id3d11device,  CreateShaderResourceView,
                     ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC)
VIEW_CREATE_OVERRIDE(id3d11device,  CreateRenderTargetView,
                     ID3D11RenderTargetView,   D3D11_RENDER_TARGET_VIEW_DESC)
VIEW_CREATE_OVERRIDE(id3d11device,  CreateDepthStencilView,
                     ID3D11DepthStencilView,   D3D11_DEPTH_STENCIL_VIEW_DESC)
VIEW_CREATE_OVERRIDE(id3d11device,  CreateUnorderedAccessView,
                     ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC)
VIEW_CREATE_OVERRIDE(id3d11device3, CreateShaderResourceView1,
                     ID3D11ShaderResourceView1, D3D11_SHADER_RESOURCE_VIEW_DESC1)
VIEW_CREATE_OVERRIDE(id3d11device3, CreateRenderTargetView1,
                     ID3D11RenderTargetView1,   D3D11_RENDER_TARGET_VIEW_DESC1)
VIEW_CREATE_OVERRIDE(id3d11device3, CreateUnorderedAccessView1,
                     ID3D11UnorderedAccessView1, D3D11_UNORDERED_ACCESS_VIEW_DESC1)


/*
 * Wrap a resource returned by View::GetResource with its concrete D3D11
 * resource interface.
 *
 * Create*View normally records the originating resource IID in the view's
 * aux.  A view obtained through another API (OMGetRenderTargets, etc.) has
 * no such record.  In that case build the generic resource wrapper once,
 * ask the host which derived resource interface it implements, remember
 * the answer on the view, and return the derived wrapper.
 */
static ID3D11Resource *
view_wrap_resource(void *self, ID3D11Resource *raw)
{
   if (!raw)
      return NULL;

   struct npt_device *dev = npt_com_self_device(self);
   struct npt_d3d11_view_aux *aux = ((struct npt_com_base *)self)->aux;
   const uint64_t raw_id = (uint64_t)(uintptr_t)raw;

   /* Fast path: Create*View, or a previous discovery, already told us
    * the concrete resource type. */
   if (aux && aux->resource_iid) {
      return (ID3D11Resource *)npt_com_get_or_wrap(
         dev, aux->resource_iid, raw_id, (struct npt_com_base *)self);
   }

   /* Start with ID3D11Resource.  The wrapper cache may already contain
    * the same host object under a concrete vtbl; detect that cheaply. */
   ID3D11Resource *base = (ID3D11Resource *)npt_com_get_or_wrap(
      dev, &NPT_IID_ID3D11Resource, raw_id, (struct npt_com_base *)self);
   if (!base)
      return NULL;

   const GUID *known = view_iid_for_resource_wrapper(base);
   if (known != &NPT_IID_ID3D11Resource) {
      if (aux)
         aux->resource_iid = known;
      return base;
   }

   /* D3D11 resources are one of these four concrete families.  Host-backed
    * QI is authoritative and npt_com already memoizes its verdicts.
    * Once found, cache the IID on the view so subsequent GetResource calls
    * take the fast path above with no QI traffic. */
   static const GUID *const candidates[] = {
      &NPT_IID_ID3D11Buffer,
      &NPT_IID_ID3D11Texture1D,
      &NPT_IID_ID3D11Texture2D,
      &NPT_IID_ID3D11Texture3D,
   };

   for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
      void *derived = NULL;
      HRESULT hr = npt_com_query_interface_host(base, candidates[i], &derived);
      if (hr == NPT_S_OK && derived) {
         if (aux)
            aux->resource_iid = candidates[i];

         /* GetResource supplied one ref for `base`; QI supplied the ref
          * that we return to the caller. */
         npt_com_default_release(base);
         return (ID3D11Resource *)derived;
      }
   }

   /* An unknown/future resource kind can still legally be exposed through
    * ID3D11Resource.  Remember that verdict to avoid probing every call. */
   if (aux)
      aux->resource_iid = &NPT_IID_ID3D11Resource;
   return base;
}

static void NPT_STDMETHODCALLTYPE
view_GetResource_override(void *self, ID3D11Resource **ppResource)
{
   ID3D11Resource *raw = NULL;
   ID3D11Resource **redirect = ppResource ? &raw : NULL;

   if (npt_com_self_device(self)->multi_ring_enabled) {
      npt_call_ID3D11View_GetResource(
         npt_com_self_ring(self), npt_com_self_id(self), redirect);
   } else {
      npt_async_ID3D11View_GetResource(
         npt_com_self_ring(self), npt_com_self_id(self), redirect);
   }

   if (!ppResource)
      return;

   *ppResource = view_wrap_resource(self, raw);
}

/* All view tiers share one aux (allocated by the runtime via
 * npt_com_register_family).  Listing every concrete tier here is
 * cheap and means each derived wrapper carries the resource-IID
 * slot regardless of which Create*View built it. */
static const GUID *const view_tiers[] = {
   &NPT_IID_ID3D11View,
   &NPT_IID_ID3D11ShaderResourceView,
   &NPT_IID_ID3D11ShaderResourceView1,
   &NPT_IID_ID3D11RenderTargetView,
   &NPT_IID_ID3D11RenderTargetView1,
   &NPT_IID_ID3D11DepthStencilView,
   &NPT_IID_ID3D11UnorderedAccessView,
   &NPT_IID_ID3D11UnorderedAccessView1,
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
npt_overrides_d3d11_view_init(void)
{
   /* Patch every Device tier's Create*View slot.  CreateShaderResourceView1
    * and friends only exist on Device3+, so they propagate from there. */
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(CreateShaderResourceView,
                                       dev_CreateShaderResourceView_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(CreateRenderTargetView,
                                       dev_CreateRenderTargetView_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(CreateDepthStencilView,
                                       dev_CreateDepthStencilView_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(CreateUnorderedAccessView,
                                       dev_CreateUnorderedAccessView_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(CreateShaderResourceView1,
                                       dev_CreateShaderResourceView1_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(CreateRenderTargetView1,
                                       dev_CreateRenderTargetView1_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(CreateUnorderedAccessView1,
                                       dev_CreateUnorderedAccessView1_override);

   /* GetResource lives on ID3D11View and is inherited by every
    * derived view tier with the same slot offset, so patching
    * each tier's storage covers all paths. */
   NPT_REGISTER_OVERRIDE(id3d11view,                   GetResource,
                         view_GetResource_override);
   NPT_REGISTER_OVERRIDE(id3d11shaderresourceview,     GetResource,
                         view_GetResource_override);
   NPT_REGISTER_OVERRIDE(id3d11shaderresourceview1,    GetResource,
                         view_GetResource_override);
   NPT_REGISTER_OVERRIDE(id3d11rendertargetview,       GetResource,
                         view_GetResource_override);
   NPT_REGISTER_OVERRIDE(id3d11rendertargetview1,      GetResource,
                         view_GetResource_override);
   NPT_REGISTER_OVERRIDE(id3d11depthstencilview,       GetResource,
                         view_GetResource_override);
   NPT_REGISTER_OVERRIDE(id3d11unorderedaccessview,    GetResource,
                         view_GetResource_override);
   NPT_REGISTER_OVERRIDE(id3d11unorderedaccessview1,   GetResource,
                         view_GetResource_override);

   npt_com_register_family(view_tiers,
                           sizeof(struct npt_d3d11_view_aux),
                           NULL);  /* default-zero aux is fine */
}
