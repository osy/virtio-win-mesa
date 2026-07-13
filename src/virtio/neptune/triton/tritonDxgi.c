/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXGI DDI handlers and base function table installer.
 *
 * MSDN: DXGI_DDI_BASE_FUNCTIONS / DXGI1_2_DDI_BASE_FUNCTIONS /
 * DXGI1_3_DDI_BASE_FUNCTIONS — function pointer signatures and the
 * IS_DXGI*_BASE_FUNCTIONS tier-detection macros live in d3d10umddi.h
 * and dxgiddi.h.
 */

#include "triton.h"
#include "triton_log.h"
#include "tritonDxgi.h"
#include "tritonPresent.h"

#include <dxgiddi.h>

/* The generated client header also emits standalone DirectX type
 * declarations, which conflict with the Windows SDK types already included
 * by triton.h under MSVC.  Keep this narrow declaration in the Triton TU
 * instead of including the generated protocol surface. */
extern HRESULT STDMETHODCALLTYPE
npt_id3d11devicecontext_default_GetData(void *self,
                                        ID3D11Asynchronous *pAsync,
                                        void *pData, UINT DataSize,
                                        UINT GetDataFlags);

#define TRITON_PRESENT_GPU_WAIT_MS 2000u

/* Windowed Present sources are exported dmabufs consumed by a different
 * virgl/KMD path.  That boundary has no implicit GPU synchronization: Flush
 * submits the producer context but may return before its writes retire.
 * Wait on an EVENT query before handing the allocation to dxgkrnl so the
 * consumer cannot copy a partially updated animation frame.
 *
 * Use the generated synchronous GetData entry directly.  Neptune's public
 * GetData override uses a feedback slot whose version advances at Begin;
 * EVENT queries are ended without Begin, so reusing one through that local
 * fast path could observe the previous Present's ready state. */
static HRESULT
tritonWaitForPresentGpu(PTRITON_DEVICE pD)
{
   if (!pD || !pD->pDev1 || !pD->pCtx1)
      return E_INVALIDARG;

   if (!pD->pPresentQuery) {
      D3D11_QUERY_DESC desc;
      memset(&desc, 0, sizeof(desc));
      desc.Query = D3D11_QUERY_EVENT;
      HRESULT hr = ID3D11Device1_CreateQuery(pD->pDev1, &desc,
                                              &pD->pPresentQuery);
      if (FAILED(hr) || !pD->pPresentQuery) {
         TR_LOG("Present GPU query creation failed: 0x%08lx", hr);
         pD->pPresentQuery = NULL;
         return FAILED(hr) ? hr : E_FAIL;
      }
   }

   ID3D11DeviceContext1_End(
      pD->pCtx1, (ID3D11Asynchronous *)pD->pPresentQuery);
   ID3D11DeviceContext1_Flush(pD->pCtx1);

   const ULONGLONG start = GetTickCount64();
   for (;;) {
      BOOL complete = FALSE;
      HRESULT hr = npt_id3d11devicecontext_default_GetData(
         pD->pCtx1, (ID3D11Asynchronous *)pD->pPresentQuery,
         &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (hr == S_OK && complete)
         return S_OK;
      if (FAILED(hr)) {
         TR_LOG("Present GPU query failed: 0x%08lx", hr);
         return hr;
      }
      if (GetTickCount64() - start >= TRITON_PRESENT_GPU_WAIT_MS) {
         TR_LOG("Present GPU query timed out after %u ms",
                TRITON_PRESENT_GPU_WAIT_MS);
         return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      }
      SwitchToThread();
   }
}

static HRESULT APIENTRY
tritonDxgiPresent(DXGI_DDI_ARG_PRESENT *pArgs)
{
   if (!pArgs) return E_INVALIDARG;

   /* The DXGI DDI uses UINT_PTR handles whose runtime value is the same
    * pDrvPrivate pointer the D3D11 DDI installs, cast to integer. */
   PTRITON_DEVICE   pD  = (PTRITON_DEVICE)(uintptr_t)pArgs->hDevice;
   PTRITON_RESOURCE src = (PTRITON_RESOURCE)(uintptr_t)pArgs->hSurfaceToPresent;
   PTRITON_RESOURCE dst = (PTRITON_RESOURCE)(uintptr_t)pArgs->hDstResource;

   if (!pD || !src) return E_INVALIDARG;
   /* Windowed flip-model devices never created a primary, so the kernel
    * context may not exist yet; create it here or the present is lost. */
   if (!pD->hKMContext)
      tritonPresentEnsureKernelContext(pD);
   if (!pD->hKMContext || !pD->pfnPresentCb || !src->hKMAllocation) {
      /* Presents must not be dropped silently in steady state; a burst
       * here means a device lost its kernel context or an allocation-less
       * surface is being presented. */
      static LONG dropCount;
      LONG n = InterlockedIncrement(&dropCount);
      if (n == 1 || (n & 1023) == 0)
         TR_LOG("Present dropped #%ld (kmalloc=0x%x %ux%u)",
                n, src->hKMAllocation, src->Width, src->Height);
      return S_OK;
   }
   /* A windowed source is shared with a separate consumer path.  Flush alone
    * is asynchronous, so wait until its producer writes are GPU-complete.
    * Display primaries stay on the existing low-latency flush path. */
   if (pD->pCtx1) {
      if (!src->IsPresentable) {
         HRESULT hr = tritonWaitForPresentGpu(pD);
         if (FAILED(hr))
            return hr;
      } else {
         ID3D11DeviceContext1_Flush(pD->pCtx1);
      }
   }

   /* DXGIDDICB_PRESENT (MSDN):
    *   hSrcAllocation  the rendered back buffer to present (a flip-primary
    *                   KM allocation in source-0).
    *   hDstAllocation  the blit destination, or 0 for a flip-style present.
    *   hContext        the device's kernel context that retires the present.
    *   pDXGIContext    runtime cookie threaded through unchanged. */
   DXGIDDICB_PRESENT cb;
   memset(&cb, 0, sizeof(cb));
   cb.hSrcAllocation = src->hKMAllocation;
   cb.hDstAllocation = dst ? dst->hKMAllocation : 0;
   cb.pDXGIContext   = pArgs->pDXGIContext;
   cb.hContext       = pD->hKMContext;
   return pD->pfnPresentCb(pD->hRTDevice.handle, &cb);
}

static HRESULT APIENTRY
tritonDxgiGetGammaCaps(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *pArgs)
{
   if (!pArgs || !pArgs->pGammaCapabilities) return E_INVALIDARG;
   /* Identity gamma — VBox's vboxDXGetGammaCaps returns the same shape:
    * one control point with scale=1, offset=0, no presence flags set. */
   DXGI_GAMMA_CONTROL_CAPABILITIES *c = pArgs->pGammaCapabilities;
   c->ScaleAndOffsetSupported = FALSE;
   c->MaxConvertedValue       = 1.0f;
   c->MinConvertedValue       = 0.0f;
   c->NumGammaControlPoints   = 0;
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiSetDisplayMode(DXGI_DDI_ARG_SETDISPLAYMODE *pArgs)
{
   if (!pArgs)
      return E_INVALIDARG;

   /* Hand the new primary to dxgkrnl.  Without this callback the
    * kernel never adopts the resource's allocation as the VidPn
    * source's primary, and every subsequent flip present completes
    * as a no-op without reaching the display miniport. */
   PTRITON_DEVICE   pD = (PTRITON_DEVICE)(uintptr_t)pArgs->hDevice;
   PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(uintptr_t)pArgs->hResource;
   if (!pD || !r)
      return E_INVALIDARG;
   if (!r->hKMAllocation || !pD->KTCallbacks.pfnSetDisplayModeCb)
      return E_INVALIDARG;

   D3DDDICB_SETDISPLAYMODE cb;
   memset(&cb, 0, sizeof(cb));
   cb.hPrimaryAllocation = r->hKMAllocation;
   return pD->KTCallbacks.pfnSetDisplayModeCb(pD->hRTDevice.handle, &cb);
}

static HRESULT APIENTRY
tritonDxgiSetResourcePriority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *pArgs)
{
   (void)pArgs;
   /* Priority is advisory; the KMD WDDM scheduler is free to ignore it. */
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiQueryResourceResidency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *pArgs)
{
   if (!pArgs || !pArgs->pStatus) return E_INVALIDARG;
   /* All allocations report FULLY_RESIDENT until the KMD says otherwise.
    * VBox does the same — no eviction tracking on the UMD side. */
   for (UINT i = 0; i < pArgs->Resources; i++)
      pArgs->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiRotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *pArgs)
{
   if (!pArgs)
      return E_INVALIDARG;
   PTRITON_DEVICE pD = (PTRITON_DEVICE)(uintptr_t)pArgs->hDevice;
   const UINT n = pArgs->Resources;
   UINT i;
   if (!pD || n <= 1 || !pArgs->pResources)
      return S_OK;

   for (i = 0; i < n; ++i) {
      PTRITON_RESOURCE r = (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[i];
      if (!r || !r->pResource) {
         TR_LOG("RotateResourceIdentities: buffer %u has no backing; "
                "flip chain left unrotated", i);
         return S_OK;
      }
   }

   /* Flip-model back buffers (windowed shared textures AND scanout
    * primaries) are DISTINCT host textures, each with its own blob-
    * backed KM allocation that consumers have imported.  Left-rotate
    * the backing tuples (resource[i] adopts resource[i+1]'s storage;
    * MSDN: "0 <= 1, 1 <= 2, etc."), keeping (host texture, allocation,
    * import rig) together so present tokens keep naming the storage
    * that was actually rendered. */
   {
      PTRITON_RESOURCE r0 = (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[0];
      ID3D11Resource *tmpRes    = r0->pResource;
      D3DKMT_HANDLE   tmpAlloc  = r0->hKMAllocation;
      BOOL            tmpShared = r0->IsShared;
      D3DKMT_HANDLE   tmpImpA   = r0->hImportAlloc;
      D3DKMT_HANDLE   tmpImpR   = r0->hImportResKmt;
      for (i = 0; i + 1 < n; ++i) {
         PTRITON_RESOURCE a = (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[i];
         PTRITON_RESOURCE b = (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[i + 1];
         a->pResource     = b->pResource;
         a->hKMAllocation = b->hKMAllocation;
         a->IsShared      = b->IsShared;
         a->hImportAlloc  = b->hImportAlloc;
         a->hImportResKmt = b->hImportResKmt;
      }
      {
         PTRITON_RESOURCE last =
            (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[n - 1];
         last->pResource     = tmpRes;
         last->hKMAllocation = tmpAlloc;
         last->IsShared      = tmpShared;
         last->hImportAlloc  = tmpImpA;
         last->hImportResKmt = tmpImpR;
      }
   }

   /* Host views wrapping these resources still target the pre-rotation
    * textures; rebuild them in place. */
   for (i = 0; i < n; ++i)
      tritonResourceRecreateViews(pD,
         (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[i]);

   /* Have the runtime replay current bindings through the DDI so the
    * host context picks up the recreated views ("the driver might be
    * required to reapply currently bound views" — MSDN; VBoxDX does the
    * same). */
   {
      const D3D11DDI_CORELAYER_DEVICECALLBACKS *cb = pD->pUMCallbacks;
      if (cb) {
         if (cb->pfnStateVsSrvCb)
            cb->pfnStateVsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStateGsSrvCb)
            cb->pfnStateGsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStatePsSrvCb)
            cb->pfnStatePsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStateHsSrvCb)
            cb->pfnStateHsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStateDsSrvCb)
            cb->pfnStateDsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStateCsSrvCb)
            cb->pfnStateCsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStateCsUavCb)
            cb->pfnStateCsUavCb(pD->hRTCoreLayer, 0,
                                D3D11_PS_CS_UAV_REGISTER_COUNT);
         if (cb->pfnStateOmRenderTargetsCb)
            cb->pfnStateOmRenderTargetsCb(pD->hRTCoreLayer);
      }
   }
   return S_OK;
}

/* Shared body for Blt and Blt1.  Blt has no explicit source rectangle
 * (the whole source subresource is the implied src); Blt1 passes one. */
static HRESULT
tritonDxgiBltCommon(DXGI_DDI_HDEVICE hDevice,
                    DXGI_DDI_HRESOURCE hDst, UINT DstSubresource,
                    UINT DstLeft, UINT DstTop, UINT DstRight, UINT DstBottom,
                    DXGI_DDI_HRESOURCE hSrc, UINT SrcSubresource,
                    UINT SrcLeft, UINT SrcTop, UINT SrcRight, UINT SrcBottom,
                    DXGI_DDI_ARG_BLT_FLAGS Flags)
{
   PTRITON_DEVICE   pD  = (PTRITON_DEVICE)(uintptr_t)hDevice;
   PTRITON_RESOURCE dst = (PTRITON_RESOURCE)(uintptr_t)hDst;
   PTRITON_RESOURCE src = (PTRITON_RESOURCE)(uintptr_t)hSrc;
   if (!pD || !pD->pCtx1 || !dst || !src || !dst->pResource || !src->pResource)
      return E_INVALIDARG;

   /* MSAA resolve blt. */
   if (Flags.Resolve) {
      ID3D11DeviceContext1_ResolveSubresource(pD->pCtx1,
                                              dst->pResource, DstSubresource,
                                              src->pResource, SrcSubresource,
                                              (DXGI_FORMAT)dst->Format);
      return S_OK;
   }

   const UINT dstW = (DstRight  > DstLeft) ? DstRight  - DstLeft : 0;
   const UINT dstH = (DstBottom > DstTop)  ? DstBottom - DstTop  : 0;

   D3D11_BOX box;
   box.left   = SrcLeft;
   box.top    = SrcTop;
   box.front  = 0;
   box.right  = SrcRight;
   box.bottom = SrcBottom;
   box.back   = 1;

   /* Stretching blts have no CopySubresourceRegion equivalent; clamp the
    * copied extent to the smaller of src box and dst rect.  DWM's
    * redirection blts are 1:1, so the clamp only fires for explicit
    * app-driven stretch (logged once). */
   if (dstW && (box.right - box.left) != dstW) {
      TR_STUB("DxgiBlt stretch width (copy clamped)");
      if (box.right - box.left > dstW)
         box.right = box.left + dstW;
   }
   if (dstH && (box.bottom - box.top) != dstH) {
      TR_STUB("DxgiBlt stretch height (copy clamped)");
      if (box.bottom - box.top > dstH)
         box.bottom = box.top + dstH;
   }

   ID3D11DeviceContext1_CopySubresourceRegion(pD->pCtx1,
                                              dst->pResource, DstSubresource,
                                              DstLeft, DstTop, 0,
                                              src->pResource, SrcSubresource,
                                              &box);
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiBlt(DXGI_DDI_ARG_BLT *pArgs)
{
   if (!pArgs)
      return E_INVALIDARG;
   PTRITON_RESOURCE src = (PTRITON_RESOURCE)(uintptr_t)pArgs->hSrcResource;
   if (!src)
      return E_INVALIDARG;
   return tritonDxgiBltCommon(pArgs->hDevice,
                              pArgs->hDstResource, pArgs->DstSubresource,
                              pArgs->DstLeft, pArgs->DstTop,
                              pArgs->DstRight, pArgs->DstBottom,
                              pArgs->hSrcResource, pArgs->SrcSubresource,
                              0, 0, src->Width, src->Height,
                              pArgs->Flags);
}

static HRESULT APIENTRY
tritonDxgiResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *pArgs)
{
   (void)pArgs;
   /* No work: cross-process shared textures are resolved to host storage
    * when opened (tritonSharedBridge), so there is nothing to resolve here. */
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiBlt1(DXGI_DDI_ARG_BLT1 *pArgs)
{
   if (!pArgs)
      return E_INVALIDARG;
   return tritonDxgiBltCommon(pArgs->hDevice,
                              pArgs->hDstResource, pArgs->DstSubresource,
                              pArgs->DstLeft, pArgs->DstTop,
                              pArgs->DstRight, pArgs->DstBottom,
                              pArgs->hSrcResource, pArgs->SrcSubresource,
                              pArgs->SrcLeft, pArgs->SrcTop,
                              pArgs->SrcRight, pArgs->SrcBottom,
                              pArgs->Flags);
}

static HRESULT APIENTRY
tritonDxgiOfferResources(DXGI_DDI_ARG_OFFERRESOURCES *pArgs)
{
   (void)pArgs;
   /* DXGI 1.2 budget-aware paging hint; advisory. */
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiReclaimResources(DXGI_DDI_ARG_RECLAIMRESOURCES *pArgs)
{
   if (!pArgs) return E_INVALIDARG;
   /* No resources are ever discarded, so every reclaim succeeds. */
   if (pArgs->pDiscarded)
      for (UINT i = 0; i < pArgs->Resources; i++)
         pArgs->pDiscarded[i] = FALSE;
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiPresent1(DXGI_DDI_ARG_PRESENT1 *pArgs)
{
   if (!pArgs)
      return E_INVALIDARG;

   /* DXGI_DDI_ARG_PRESENT1 (MSDN):
    *   phSurfacesToPresent[0..SurfacesToPresent-1]  source surfaces (the
    *     first is always the primary plane; secondary planes are stereo).
    *   hDstResource                                  blit destination or 0. */
   PTRITON_DEVICE pD = (PTRITON_DEVICE)(uintptr_t)pArgs->hDevice;
   if (!pD || !pArgs->SurfacesToPresent || !pArgs->phSurfacesToPresent)
      return E_INVALIDARG;
   PTRITON_RESOURCE src =
      (PTRITON_RESOURCE)(uintptr_t)pArgs->phSurfacesToPresent[0].hSurface;
   PTRITON_RESOURCE dst = (PTRITON_RESOURCE)(uintptr_t)pArgs->hDstResource;
   if (!src) return E_INVALIDARG;
   if (!pD->hKMContext)
      tritonPresentEnsureKernelContext(pD);
   if (!pD->hKMContext || !pD->pfnPresentCb || !src->hKMAllocation) {
      static LONG dropCount1;
      LONG n = InterlockedIncrement(&dropCount1);
      if (n == 1 || (n & 1023) == 0)
         TR_LOG("Present1 dropped #%ld (kmalloc=0x%x %ux%u)",
                n, src->hKMAllocation, src->Width, src->Height);
      return S_OK;
   }
   /* See tritonDxgiPresent: windowed shared sources require producer
    * GPU completion, not merely an asynchronous Flush. */
   if (!src->IsPresentable && pD->pCtx1) {
      HRESULT hr = tritonWaitForPresentGpu(pD);
      if (FAILED(hr))
         return hr;
   }

   DXGIDDICB_PRESENT cb;
   memset(&cb, 0, sizeof(cb));
   cb.hSrcAllocation = src->hKMAllocation;
   cb.hDstAllocation = dst ? dst->hKMAllocation : 0;
   cb.pDXGIContext   = pArgs->pDXGIContext;
   cb.hContext       = pD->hKMContext;
   return pD->pfnPresentCb(pD->hRTDevice.handle, &cb);
}

static HRESULT APIENTRY
tritonDxgiCheckPresentDurationSupport(DXGI_DDI_ARG_CHECKPRESENTDURATIONSUPPORT *pArgs)
{
   if (!pArgs) return E_INVALIDARG;
   /* No custom present-duration support — runtime falls back to vblank. */
   pArgs->ClosestSmallerDuration = 0;
   pArgs->ClosestLargerDuration  = 0;
   return S_OK;
}

/* ---------- Installer ---------- */

/* Cover the lowest-tier base function table fields (DXGI_DDI_BASE_FUNCTIONS).
 * Each later variant strictly extends this prefix layout, so a tier-1
 * fill is the foundation for tier-2 / tier-3 / tier-4 etc. */
static void
triton_fill_base(DXGI_DDI_BASE_FUNCTIONS *p)
{
   p->pfnPresent                = tritonDxgiPresent;
   p->pfnGetGammaCaps           = tritonDxgiGetGammaCaps;
   p->pfnSetDisplayMode         = tritonDxgiSetDisplayMode;
   p->pfnSetResourcePriority    = tritonDxgiSetResourcePriority;
   p->pfnQueryResourceResidency = tritonDxgiQueryResourceResidency;
   p->pfnRotateResourceIdentities = tritonDxgiRotateResourceIdentities;
   p->pfnBlt                    = tritonDxgiBlt;
}

static void
triton_fill_1_1(DXGI1_1_DDI_BASE_FUNCTIONS *p)
{
   triton_fill_base((DXGI_DDI_BASE_FUNCTIONS *)p);
   p->pfnResolveSharedResource = tritonDxgiResolveSharedResource;
}

static void
triton_fill_1_2(DXGI1_2_DDI_BASE_FUNCTIONS *p)
{
   triton_fill_1_1((DXGI1_1_DDI_BASE_FUNCTIONS *)p);
   p->pfnBlt1                            = tritonDxgiBlt1;
   p->pfnOfferResources                  = tritonDxgiOfferResources;
   p->pfnReclaimResources                = tritonDxgiReclaimResources;
   /* Multiplane-overlay entries stay NULL: MPO is unsupported, and a
    * NULL pfnGetMultiplaneOverlayCaps is how the runtime detects that.
    * DWM then presents through the ordinary flip path (pfnPresent1 +
    * pfnRotateResourceIdentities), same as VBoxDX. */
   p->pfnGetMultiplaneOverlayCaps        = NULL;
   p->pfnGetMultiplaneOverlayFilterRange = NULL;
   p->pfnCheckMultiplaneOverlaySupport   = NULL;
   p->pfnPresentMultiplaneOverlay        = NULL;
}

static void
triton_fill_1_3(DXGI1_3_DDI_BASE_FUNCTIONS *p)
{
   p->pfnPresent                = tritonDxgiPresent;
   p->pfnGetGammaCaps           = tritonDxgiGetGammaCaps;
   p->pfnSetDisplayMode         = tritonDxgiSetDisplayMode;
   p->pfnSetResourcePriority    = tritonDxgiSetResourcePriority;
   p->pfnQueryResourceResidency = tritonDxgiQueryResourceResidency;
   p->pfnRotateResourceIdentities = tritonDxgiRotateResourceIdentities;
   p->pfnBlt                    = tritonDxgiBlt;
   p->pfnResolveSharedResource  = tritonDxgiResolveSharedResource;
   p->pfnBlt1                   = tritonDxgiBlt1;
   p->pfnOfferResources         = tritonDxgiOfferResources;
   p->pfnReclaimResources       = tritonDxgiReclaimResources;
   /* Multiplane-overlay entries stay NULL (unsupported); see
    * triton_fill_1_2. */
   p->pfnGetMultiplaneOverlayCaps      = NULL;
   p->pfnGetMultiplaneOverlayGroupCaps = NULL;
   p->pfnReserved1                     = NULL;
   p->pfnPresentMultiplaneOverlay      = NULL;
   p->pfnReserved2                     = NULL;
   p->pfnPresent1                      = tritonDxgiPresent1;
   p->pfnCheckPresentDurationSupport   = tritonDxgiCheckPresentDurationSupport;
}

void
tritonInstallDXGIFuncs(PTRITON_DEVICE pD,
                       const D3D10DDIARG_CREATEDEVICE *pArgs)
{
   if (!pArgs || !pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions) {
      TR_LOG("InstallDXGIFuncs: no DXGI base table pointer (interface 0x%08x)",
             pArgs ? pArgs->Interface : 0);
      return;
   }

   /* Record the runtime's present callback. tritonDxgiPresent invokes it
    * (with the kernel context) to submit the flip/blit to dxgkrnl, which
    * drives the KMD's scanout. The base callbacks pointer is an IN field
    * the runtime asks us to retain (dxgiddi.h DXGI_DDI_BASE_ARGS). */
   if (pD && pArgs->DXGIBaseDDI.pDXGIBaseCallbacks)
      pD->pfnPresentCb = pArgs->DXGIBaseDDI.pDXGIBaseCallbacks->pfnPresentCb;

   /* Pick the tier matching the runtime-requested DDI interface.  The
    * SDK overlays the version-specific pointers as a union inside
    * DXGI_DDI_BASE_ARGS, so the chosen field aliases the same storage. */
   if (IS_DXGI1_3_BASE_FUNCTIONS(pArgs->Interface, pArgs->Version)) {
      triton_fill_1_3(pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions4);
      TR_LOG("InstallDXGIFuncs: tier DXGI1_3");
   } else if (IS_DXGI1_2_BASE_FUNCTIONS(pArgs->Interface, pArgs->Version)) {
      triton_fill_1_2(pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3);
      TR_LOG("InstallDXGIFuncs: tier DXGI1_2");
   } else {
      /* Pre-DXGI-1.1 callers fall to the base table. */
      triton_fill_base(pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions);
      TR_LOG("InstallDXGIFuncs: tier DXGI1_0");
   }
}
