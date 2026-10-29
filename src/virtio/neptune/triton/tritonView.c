/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * SRV / RTV / DSV / UAV create + destroy, clear handlers, flush,
 * SetRenderTargets, and per-stage SRV/UAV binding.
 *
 * View dimensions are picked from the RESOURCE, not from the view: a
 * single-slice view (ArraySize=1) of an array resource still needs an
 * *ARRAY dimension to expose FirstArraySlice, and a cube resource is a
 * cube array iff ArraySize > 6 (a cube is 6 faces). D3D11 has no cube
 * RTV/DSV dimension, so cubes bind as TEXTURE2DARRAY with the faces as
 * array slices.
 */

#include "triton.h"
#include "triton_log.h"

/* ---------- per-resource view list + rotation support ---------- */

void
tritonResourceLinkView(PTRITON_RESOURCE r, TRITON_VIEWLINK *l,
                       TRITON_VIEW_KIND kind)
{
    l->kind   = kind;
    l->pNext  = r->pViewList;
    l->ppPrev = &r->pViewList;
    if (r->pViewList)
        r->pViewList->ppPrev = &l->pNext;
    r->pViewList = l;
}

void
tritonViewUnlink(TRITON_VIEWLINK *l)
{
    if (!l->ppPrev)
        return;
    *l->ppPrev = l->pNext;
    if (l->pNext)
        l->pNext->ppPrev = l->ppPrev;
    l->pNext  = NULL;
    l->ppPrev = NULL;
}

/* Recreate every host view of r against its (rotated) host resource.
 * The wrapper objects and their DDI handles stay put; only the wrapped
 * host COM view is replaced, from the desc each view stored at create time
 * (recovering it with GetDesc would be a synchronous host round trip per
 * view on every flip-model present).  Views created through the WDDM 2.0
 * *View1 paths are recreated the same way, keeping PlaneSlice. */
HRESULT
tritonResourceRecreateViews(PTRITON_DEVICE pD, PTRITON_RESOURCE r)
{
    HRESULT worst = S_OK;
    TRITON_VIEWLINK *l;
    for (l = r->pViewList; l; l = l->pNext) {
        HRESULT hr = S_OK;
        switch (l->kind) {
        case TRITON_VIEW_RTV: {
            PTRITON_RTVIEW v = CONTAINING_RECORD(l, TRITON_RTVIEW, link);
            if (!v->pRTV) break;
            ID3D11RenderTargetView_Release(v->pRTV);
            v->pRTV = NULL;
            if (v->fDesc1) {
                ID3D11RenderTargetView1 *p1 = NULL;
                hr = ID3D11Device3_CreateRenderTargetView1(pD->pDev3, r->pResource,
                                                           &v->Desc1, &p1);
                v->pRTV = (ID3D11RenderTargetView *)p1;
            } else {
                hr = ID3D11Device1_CreateRenderTargetView(pD->pDev1, r->pResource,
                                                          &v->Desc, &v->pRTV);
            }
            break;
        }
        case TRITON_VIEW_SRV: {
            PTRITON_SRVIEW v = CONTAINING_RECORD(l, TRITON_SRVIEW, link);
            if (!v->pSRV) break;
            ID3D11ShaderResourceView_Release(v->pSRV);
            v->pSRV = NULL;
            if (v->fDesc1) {
                ID3D11ShaderResourceView1 *p1 = NULL;
                hr = ID3D11Device3_CreateShaderResourceView1(pD->pDev3, r->pResource,
                                                             &v->Desc1, &p1);
                v->pSRV = (ID3D11ShaderResourceView *)p1;
            } else {
                hr = ID3D11Device1_CreateShaderResourceView(pD->pDev1, r->pResource,
                                                            &v->Desc, &v->pSRV);
            }
            break;
        }
        case TRITON_VIEW_DSV: {
            PTRITON_DSVIEW v = CONTAINING_RECORD(l, TRITON_DSVIEW, link);
            if (!v->pDSV) break;
            ID3D11DepthStencilView_Release(v->pDSV);
            v->pDSV = NULL;
            hr = ID3D11Device1_CreateDepthStencilView(pD->pDev1, r->pResource,
                                                      &v->Desc, &v->pDSV);
            break;
        }
        case TRITON_VIEW_UAV: {
            PTRITON_UAVIEW v = CONTAINING_RECORD(l, TRITON_UAVIEW, link);
            if (!v->pUAV) break;
            ID3D11UnorderedAccessView_Release(v->pUAV);
            v->pUAV = NULL;
            if (v->fDesc1) {
                ID3D11UnorderedAccessView1 *p1 = NULL;
                hr = ID3D11Device3_CreateUnorderedAccessView1(pD->pDev3, r->pResource,
                                                              &v->Desc1, &p1);
                v->pUAV = (ID3D11UnorderedAccessView *)p1;
            } else {
                hr = ID3D11Device1_CreateUnorderedAccessView(pD->pDev1, r->pResource,
                                                             &v->Desc, &v->pUAV);
            }
            break;
        }
        }
        if (FAILED(hr)) {
            TR_LOG("RecreateViews: kind=%d hr=0x%08lx", l->kind, hr);
            worst = hr;
        }
    }
    return worst;
}

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateRenderTargetViewSize, TRITON_RTVIEW,
                     const D3D10DDIARG_CREATERENDERTARGETVIEW *pArgs)

/* The 11.x create args are a prefix of the WDDM 2.0 ones: the Tex2D member
 * only gains a trailing PlaneSlice, and no other member reaches past it, so
 * the bytes before PlaneSlice carry every 11.x field. */
#define TRITON_RTV_ARGS11_BYTES offsetof(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Tex2D.PlaneSlice)
_Static_assert(offsetof(D3D10DDIARG_CREATERENDERTARGETVIEW, Tex2D) == offsetof(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Tex2D) &&
               offsetof(D3D10DDIARG_CREATERENDERTARGETVIEW, Tex2D) + sizeof(((D3D10DDIARG_CREATERENDERTARGETVIEW *)0)->Tex2D) ==
                   TRITON_RTV_ARGS11_BYTES &&
               TRITON_RTV_ARGS11_BYTES <= sizeof(D3D10DDIARG_CREATERENDERTARGETVIEW),
               "RTV create args are not prefix-compatible");
_Static_assert(sizeof(((D3D10DDIARG_CREATERENDERTARGETVIEW *)0)->Buffer) <= sizeof(((D3D10DDIARG_CREATERENDERTARGETVIEW *)0)->Tex2D),
               "RTV create arg union outgrew Tex2D");
_Static_assert(sizeof(((D3D10DDIARG_CREATERENDERTARGETVIEW *)0)->Tex1D) <= sizeof(((D3D10DDIARG_CREATERENDERTARGETVIEW *)0)->Tex2D),
               "RTV create arg union outgrew Tex2D");
_Static_assert(sizeof(((D3D10DDIARG_CREATERENDERTARGETVIEW *)0)->Tex3D) <= sizeof(((D3D10DDIARG_CREATERENDERTARGETVIEW *)0)->Tex2D),
               "RTV create arg union outgrew Tex2D");
_Static_assert(sizeof(((D3D10DDIARG_CREATERENDERTARGETVIEW *)0)->TexCube) <= sizeof(((D3D10DDIARG_CREATERENDERTARGETVIEW *)0)->Tex2D),
               "RTV create arg union outgrew Tex2D");

static BOOL
tritonBuildRtvDesc(const TRITON_RESOURCE *r, const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *a,
                   D3D11_RENDER_TARGET_VIEW_DESC1 *d)
{
    memset(d, 0, sizeof(*d));
    d->Format = a->Format;
    const BOOL fIsArray = (r->ArraySize > 1);
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        d->ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
        d->Buffer.FirstElement = a->Buffer.FirstElement;
        d->Buffer.NumElements  = a->Buffer.NumElements;
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (fIsArray) {
            d->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
            d->Texture1DArray.MipSlice        = a->Tex1D.MipSlice;
            d->Texture1DArray.FirstArraySlice = a->Tex1D.FirstArraySlice;
            d->Texture1DArray.ArraySize       = a->Tex1D.ArraySize;
        } else {
            d->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
            d->Texture1D.MipSlice = a->Tex1D.MipSlice;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        if (r->SampleDesc.Count > 1) {
            if (fIsArray) {
                d->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                d->Texture2DMSArray.FirstArraySlice = a->Tex2D.FirstArraySlice;
                d->Texture2DMSArray.ArraySize       = a->Tex2D.ArraySize;
            } else {
                d->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
            }
        } else if (fIsArray) {
            d->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            d->Texture2DArray.MipSlice        = a->Tex2D.MipSlice;
            d->Texture2DArray.FirstArraySlice = a->Tex2D.FirstArraySlice;
            d->Texture2DArray.ArraySize       = a->Tex2D.ArraySize;
            d->Texture2DArray.PlaneSlice      = a->Tex2D.PlaneSlice;
        } else {
            d->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            d->Texture2D.MipSlice    = a->Tex2D.MipSlice;
            d->Texture2D.PlaneSlice  = a->Tex2D.PlaneSlice;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        d->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
        d->Texture3D.MipSlice    = a->Tex3D.MipSlice;
        d->Texture3D.FirstWSlice = a->Tex3D.FirstW;
        d->Texture3D.WSize       = a->Tex3D.WSize;
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        d->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        d->Texture2DArray.MipSlice        = a->TexCube.MipSlice;
        d->Texture2DArray.FirstArraySlice = a->TexCube.FirstArraySlice;
        d->Texture2DArray.ArraySize       = a->TexCube.ArraySize;
        break;
    default:
        TR_LOG("CreateRenderTargetView: unknown dimension %d", a->ResourceDimension);
        return FALSE;
    }
    return TRUE;
}

/* Views are created through the 11.3 *View1 call when the host exposes it,
 * so PlaneSlice reaches it; otherwise through the base call, which takes the
 * DESC1 with its trailing PlaneSlice fields dropped (DESC is a prefix of
 * DESC1 member by member). */
static void
tritonCreateRtvCommon(PTRITON_DEVICE pD, TRITON_RTVIEW *v, const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *a)
{
    PTRITON_RESOURCE r = (PTRITON_RESOURCE)(a->hDrvResource.pDrvPrivate);
    if (!pD || !v || !r || !r->pResource) { tritonSetCreateError(pD, E_INVALIDARG); return; }
    v->pResource = r;
    v->pRTV = NULL;

    D3D11_RENDER_TARGET_VIEW_DESC1 d;
    if (!tritonBuildRtvDesc(r, a, &d)) {
        tritonSetCreateError(pD, E_INVALIDARG);
        return;
    }
    HRESULT hr;
    if (pD->pDev3) {
        ID3D11RenderTargetView1 *p1 = NULL;
        v->fDesc1 = TRUE;
        v->Desc1  = d;
        hr = ID3D11Device3_CreateRenderTargetView1(pD->pDev3, r->pResource, &d, &p1);
        v->pRTV = (ID3D11RenderTargetView *)p1;
    } else {
        v->fDesc1 = FALSE;
        memcpy(&v->Desc, &d, sizeof(v->Desc));
        hr = ID3D11Device1_CreateRenderTargetView(pD->pDev1, r->pResource, &v->Desc, &v->pRTV);
    }
    if (FAILED(hr) || !v->pRTV) {
        TR_LOG("CreateRenderTargetView: 0x%08lx", hr);
        v->pRTV = NULL;
        tritonSetCreateError(pD, hr);
        return;
    }
    tritonResourceLinkView(r, &v->link, TRITON_VIEW_RTV);
}

void APIENTRY
tritonCreateRenderTargetView(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_CREATERENDERTARGETVIEW *pArgs,
                             D3D10DDI_HRENDERTARGETVIEW hView, D3D10DDI_HRTRENDERTARGETVIEW hRTView)
{
    (void)hRTView;
    D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW a;
    memset(&a, 0, sizeof(a));
    memcpy(&a, pArgs, TRITON_RTV_ARGS11_BYTES);
    tritonCreateRtvCommon((PTRITON_DEVICE)hDevice.pDrvPrivate,
                          (TRITON_RTVIEW *)hView.pDrvPrivate, &a);
}

void APIENTRY
tritonCreateRenderTargetView_WDDM2_0(D3D10DDI_HDEVICE hDevice, const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *pArgs,
                                     D3D10DDI_HRENDERTARGETVIEW hView, D3D10DDI_HRTRENDERTARGETVIEW hRTView)
{
    (void)hRTView;
    tritonCreateRtvCommon((PTRITON_DEVICE)hDevice.pDrvPrivate,
                          (TRITON_RTVIEW *)hView.pDrvPrivate, pArgs);
}

void APIENTRY
tritonDestroyRenderTargetView(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRENDERTARGETVIEW hRTV)
{
    (void)hDevice;
    PTRITON_RTVIEW v = (PTRITON_RTVIEW)(hRTV.pDrvPrivate);
    if (!v) return;
    tritonViewUnlink(&v->link);
    if (v->pRTV) { ID3D11RenderTargetView_Release(v->pRTV); v->pRTV = NULL; }
    v->pResource = NULL;
}

void APIENTRY
tritonClearRenderTargetView(D3D10DDI_HDEVICE hDevice,
                            D3D10DDI_HRENDERTARGETVIEW hRTV,
                            FLOAT color[4])
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RTVIEW v  = (PTRITON_RTVIEW)(hRTV.pDrvPrivate);
    if (!pD || !v || !v->pRTV) return;
    ID3D11DeviceContext1_ClearRenderTargetView(pD->pCtx1, v->pRTV, color);
}

void APIENTRY
tritonClearDepthStencilView(D3D10DDI_HDEVICE hDevice,
                            D3D10DDI_HDEPTHSTENCILVIEW hDSV,
                            UINT Flags, FLOAT Depth, UINT8 Stencil)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_DSVIEW v  = (PTRITON_DSVIEW)(hDSV.pDrvPrivate);
    if (!pD || !v || !v->pDSV) return;
    /* DDI uses identical bit layout to D3D11_CLEAR_DEPTH/_STENCIL. */
    ID3D11DeviceContext1_ClearDepthStencilView(pD->pCtx1, v->pDSV, Flags, Depth, Stencil);
}

void APIENTRY
tritonFlush(D3D10DDI_HDEVICE hDevice)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD || !pD->pCtx1) return;
    ID3D11DeviceContext1_Flush(pD->pCtx1);
}

BOOL APIENTRY
tritonFlush11_1(D3D10DDI_HDEVICE hDevice, UINT FlushFlags)
{
    (void)FlushFlags;
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD || !pD->pCtx1) return FALSE;
    ID3D11DeviceContext1_Flush(pD->pCtx1);
    return TRUE;
}

void APIENTRY
tritonSetRenderTargets(D3D10DDI_HDEVICE hDevice,
                       const D3D10DDI_HRENDERTARGETVIEW *phRTVs,
                       UINT NumRTVs,
                       UINT ClearSlots,
                       D3D10DDI_HDEPTHSTENCILVIEW hDSV,
                       const D3D11DDI_HUNORDEREDACCESSVIEW *phUAVs,
                       const UINT *pUAVInitialCounts,
                       UINT UAVStartSlot,
                       UINT NumUAVs,
                       UINT UAVRangeStart,
                       UINT UAVRangeSize)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD || !pD->pCtx1) return;

    /* `ClearSlots` (DDI) signals additional slots beyond NumRTVs that
     * were previously bound and should be unbound. D3D11's
     * `OMSetRenderTargets*` already auto-unbinds slots beyond the
     * supplied NumViews, so we pass NumRTVs and let D3D11 handle it.
     * Passing NumRTVs+ClearSlots would conflict with `UAVStartSlot`
     * (defined as >= NumRTVs but may be < NumRTVs+ClearSlots). */
    if (NumRTVs > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) {
        TR_LOG("SetRenderTargets: NumRTVs %u exceeds max %u",
               NumRTVs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
        return;
    }
    (void)ClearSlots;
    ID3D11RenderTargetView *aRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    for (UINT i = 0; i < NumRTVs; ++i) {
        PTRITON_RTVIEW v = phRTVs
            ? (PTRITON_RTVIEW)(phRTVs[i].pDrvPrivate)
            : NULL;
        aRTV[i] = v ? v->pRTV : NULL;
    }

    PTRITON_DSVIEW dsv = (PTRITON_DSVIEW)(hDSV.pDrvPrivate);
    ID3D11DepthStencilView *pDSV = dsv ? dsv->pDSV : NULL;

    /* The DDI always binds everything ("the Direct3D DDI always binds
     * everything", SetRenderTargets(D3D11)), so NumUAVs == 0 means no OM
     * UAVs.  Say so explicitly: whether a plain OMSetRenderTargets clears
     * previously bound UAVs differs between host backends.  UAVRangeStart
     * and UAVRangeSize are change hints only. */
    (void)UAVRangeStart; (void)UAVRangeSize;
    if (NumUAVs == 0)
        UAVStartSlot = NumRTVs;
    if (UAVStartSlot > D3D11_1_UAV_SLOT_COUNT ||
        NumUAVs > D3D11_1_UAV_SLOT_COUNT - UAVStartSlot)
        return;
    ID3D11UnorderedAccessView *aUAV[D3D11_1_UAV_SLOT_COUNT] = {};
    for (UINT i = 0; i < NumUAVs; ++i) {
        PTRITON_UAVIEW u = phUAVs
            ? (PTRITON_UAVIEW)(phUAVs[i].pDrvPrivate)
            : NULL;
        aUAV[i] = u ? u->pUAV : NULL;
    }
    ID3D11DeviceContext1_OMSetRenderTargetsAndUnorderedAccessViews(
        pD->pCtx1, NumRTVs, aRTV, pDSV,
        UAVStartSlot, NumUAVs, aUAV, NumUAVs ? pUAVInitialCounts : NULL);

    BOOL hasTargets = pDSV != NULL;
    for (UINT i = 0; i < NumRTVs && !hasTargets; ++i)
        hasTargets = aRTV[i] != NULL;
    if (hasTargets != pD->omHasTargets) {
        pD->omHasTargets = hasTargets;
        if (pD->omBlendSet && pD->omSampleMask != 0xFFFFFFFFu)
            tritonApplyBlendState(pD);
    }
    tritonUpdateTirPixelShader(pD);
}

/* ---------- ShaderResourceView ---------- */

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateSRVSize, TRITON_SRVIEW,
                     const D3D11DDIARG_CREATESHADERRESOURCEVIEW *pArgs)

/* The 11.x create args are a prefix of the WDDM 2.0 ones: the Tex2D member
 * only gains a trailing PlaneSlice, and no other member reaches past it, so
 * the bytes before PlaneSlice carry every 11.x field. */
#define TRITON_SRV_ARGS11_BYTES offsetof(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Tex2D.PlaneSlice)
_Static_assert(offsetof(D3D11DDIARG_CREATESHADERRESOURCEVIEW, Tex2D) == offsetof(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Tex2D) &&
               offsetof(D3D11DDIARG_CREATESHADERRESOURCEVIEW, Tex2D) + sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->Tex2D) ==
                   TRITON_SRV_ARGS11_BYTES &&
               TRITON_SRV_ARGS11_BYTES <= sizeof(D3D11DDIARG_CREATESHADERRESOURCEVIEW),
               "SRV create args are not prefix-compatible");
_Static_assert(sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->Buffer) <= sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->Tex2D),
               "SRV create arg union outgrew Tex2D");
_Static_assert(sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->Tex1D) <= sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->Tex2D),
               "SRV create arg union outgrew Tex2D");
_Static_assert(sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->Tex3D) <= sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->Tex2D),
               "SRV create arg union outgrew Tex2D");
_Static_assert(sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->TexCube) <= sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->Tex2D),
               "SRV create arg union outgrew Tex2D");
_Static_assert(sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->BufferEx) <= sizeof(((D3D11DDIARG_CREATESHADERRESOURCEVIEW *)0)->Tex2D),
               "SRV create arg union outgrew Tex2D");

static BOOL
tritonBuildSrvDesc(const TRITON_RESOURCE *r, const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *a,
                   D3D11_SHADER_RESOURCE_VIEW_DESC1 *d)
{
    memset(d, 0, sizeof(*d));
    d->Format = a->Format;
    const BOOL fIsArray     = (r->ArraySize > 1);
    const BOOL fIsCubeArray = (r->ArraySize > 6);
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        d->ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        d->Buffer.FirstElement = a->Buffer.FirstElement;
        d->Buffer.NumElements  = a->Buffer.NumElements;
        break;
    case D3D11DDIRESOURCE_BUFFEREX:
        d->ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        d->BufferEx.FirstElement = a->BufferEx.FirstElement;
        d->BufferEx.NumElements  = a->BufferEx.NumElements;
        d->BufferEx.Flags        = a->BufferEx.Flags;
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (fIsArray) {
            d->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
            d->Texture1DArray.MostDetailedMip = a->Tex1D.MostDetailedMip;
            d->Texture1DArray.MipLevels       = a->Tex1D.MipLevels;
            d->Texture1DArray.FirstArraySlice = a->Tex1D.FirstArraySlice;
            d->Texture1DArray.ArraySize       = a->Tex1D.ArraySize;
        } else {
            d->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
            d->Texture1D.MostDetailedMip = a->Tex1D.MostDetailedMip;
            d->Texture1D.MipLevels       = a->Tex1D.MipLevels;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        if (r->SampleDesc.Count > 1) {
            if (fIsArray) {
                d->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                d->Texture2DMSArray.FirstArraySlice = a->Tex2D.FirstArraySlice;
                d->Texture2DMSArray.ArraySize       = a->Tex2D.ArraySize;
            } else {
                d->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            }
        } else if (fIsArray) {
            d->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            d->Texture2DArray.MostDetailedMip = a->Tex2D.MostDetailedMip;
            d->Texture2DArray.MipLevels       = a->Tex2D.MipLevels;
            d->Texture2DArray.FirstArraySlice = a->Tex2D.FirstArraySlice;
            d->Texture2DArray.ArraySize       = a->Tex2D.ArraySize;
            d->Texture2DArray.PlaneSlice      = a->Tex2D.PlaneSlice;
        } else {
            d->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            d->Texture2D.MostDetailedMip = a->Tex2D.MostDetailedMip;
            d->Texture2D.MipLevels       = a->Tex2D.MipLevels;
            d->Texture2D.PlaneSlice      = a->Tex2D.PlaneSlice;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        d->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        d->Texture3D.MostDetailedMip = a->Tex3D.MostDetailedMip;
        d->Texture3D.MipLevels       = a->Tex3D.MipLevels;
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        if (fIsCubeArray) {
            d->ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            d->TextureCubeArray.MostDetailedMip = a->TexCube.MostDetailedMip;
            d->TextureCubeArray.MipLevels       = a->TexCube.MipLevels;
            d->TextureCubeArray.First2DArrayFace= a->TexCube.First2DArrayFace;
            d->TextureCubeArray.NumCubes        = a->TexCube.NumCubes;
        } else {
            d->ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            d->TextureCube.MostDetailedMip = a->TexCube.MostDetailedMip;
            d->TextureCube.MipLevels       = a->TexCube.MipLevels;
        }
        break;
    default:
        TR_LOG("CreateSRV: unknown dimension %d", a->ResourceDimension);
        return FALSE;
    }
    return TRUE;
}

/* Views are created through the 11.3 *View1 call when the host exposes it,
 * so PlaneSlice reaches it; otherwise through the base call, which takes the
 * DESC1 with its trailing PlaneSlice fields dropped (DESC is a prefix of
 * DESC1 member by member). */
static void
tritonCreateSrvCommon(PTRITON_DEVICE pD, TRITON_SRVIEW *v, const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *a)
{
    PTRITON_RESOURCE r = (PTRITON_RESOURCE)(a->hDrvResource.pDrvPrivate);
    if (!pD || !v || !r || !r->pResource) { tritonSetCreateError(pD, E_INVALIDARG); return; }
    v->pResource = r;
    v->pSRV = NULL;

    D3D11_SHADER_RESOURCE_VIEW_DESC1 d;
    if (!tritonBuildSrvDesc(r, a, &d)) {
        tritonSetCreateError(pD, E_INVALIDARG);
        return;
    }
    HRESULT hr;
    if (pD->pDev3) {
        ID3D11ShaderResourceView1 *p1 = NULL;
        v->fDesc1 = TRUE;
        v->Desc1  = d;
        hr = ID3D11Device3_CreateShaderResourceView1(pD->pDev3, r->pResource, &d, &p1);
        v->pSRV = (ID3D11ShaderResourceView *)p1;
    } else {
        v->fDesc1 = FALSE;
        memcpy(&v->Desc, &d, sizeof(v->Desc));
        hr = ID3D11Device1_CreateShaderResourceView(pD->pDev1, r->pResource, &v->Desc, &v->pSRV);
    }
    if (FAILED(hr) || !v->pSRV) {
        TR_LOG("CreateSRV: 0x%08lx", hr);
        v->pSRV = NULL;
        tritonSetCreateError(pD, hr);
        return;
    }
    tritonResourceLinkView(r, &v->link, TRITON_VIEW_SRV);
}

void APIENTRY
tritonCreateSRV(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATESHADERRESOURCEVIEW *pArgs,
                D3D10DDI_HSHADERRESOURCEVIEW hView, D3D10DDI_HRTSHADERRESOURCEVIEW hRTView)
{
    (void)hRTView;
    D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW a;
    memset(&a, 0, sizeof(a));
    memcpy(&a, pArgs, TRITON_SRV_ARGS11_BYTES);
    tritonCreateSrvCommon((PTRITON_DEVICE)hDevice.pDrvPrivate,
                          (TRITON_SRVIEW *)hView.pDrvPrivate, &a);
}

void APIENTRY
tritonCreateSRV_WDDM2_0(D3D10DDI_HDEVICE hDevice, const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *pArgs,
                        D3D10DDI_HSHADERRESOURCEVIEW hView, D3D10DDI_HRTSHADERRESOURCEVIEW hRTView)
{
    (void)hRTView;
    tritonCreateSrvCommon((PTRITON_DEVICE)hDevice.pDrvPrivate,
                          (TRITON_SRVIEW *)hView.pDrvPrivate, pArgs);
}

void APIENTRY
tritonDestroySRV(D3D10DDI_HDEVICE hDev, D3D10DDI_HSHADERRESOURCEVIEW hSRV)
{
    PTRITON_SRVIEW v = (PTRITON_SRVIEW)(hSRV.pDrvPrivate);
    if (!v) return;
    tritonViewUnlink(&v->link);
    if (v->pSRV) { ID3D11ShaderResourceView_Release(v->pSRV); v->pSRV = NULL; }
    v->pResource = NULL;
}

/* ---------- DepthStencilView ---------- */

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateDSVSize, TRITON_DSVIEW,
                     const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *pArgs)

void APIENTRY
tritonCreateDSV(D3D10DDI_HDEVICE hDevice,
                const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *pArgs,
                D3D10DDI_HDEPTHSTENCILVIEW hDSV,
                D3D10DDI_HRTDEPTHSTENCILVIEW hRTDSV)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_DSVIEW   v  = (PTRITON_DSVIEW)(hDSV.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(pArgs->hDrvResource.pDrvPrivate);
    if (!pD || !v || !r || !r->pResource) { tritonSetCreateError(pD, E_INVALIDARG); return; }
    v->pResource = r;
    v->pDSV      = NULL;

    D3D11_DEPTH_STENCIL_VIEW_DESC d = {};
    d.Format = pArgs->Format;
    d.Flags  = pArgs->Flags;
    const BOOL fIsArray = (r->ArraySize > 1);
    switch (pArgs->ResourceDimension) {
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (fIsArray) {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray.MipSlice        = pArgs->Tex1D.MipSlice;
            d.Texture1DArray.FirstArraySlice = pArgs->Tex1D.FirstArraySlice;
            d.Texture1DArray.ArraySize       = pArgs->Tex1D.ArraySize;
        } else {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
            d.Texture1D.MipSlice = pArgs->Tex1D.MipSlice;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        if (r->SampleDesc.Count > 1) {
            if (fIsArray) {
                d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
                d.Texture2DMSArray.FirstArraySlice = pArgs->Tex2D.FirstArraySlice;
                d.Texture2DMSArray.ArraySize       = pArgs->Tex2D.ArraySize;
            } else {
                d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
            }
        } else if (fIsArray) {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.MipSlice        = pArgs->Tex2D.MipSlice;
            d.Texture2DArray.FirstArraySlice = pArgs->Tex2D.FirstArraySlice;
            d.Texture2DArray.ArraySize       = pArgs->Tex2D.ArraySize;
        } else {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipSlice = pArgs->Tex2D.MipSlice;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MipSlice        = pArgs->TexCube.MipSlice;
        d.Texture2DArray.FirstArraySlice = pArgs->TexCube.FirstArraySlice;
        d.Texture2DArray.ArraySize       = pArgs->TexCube.ArraySize;
        break;
    default:
        TR_LOG("CreateDSV: unknown dimension %d", pArgs->ResourceDimension);
        tritonSetCreateError(pD, E_INVALIDARG);
        return;
    }
    v->Desc = d;
    HRESULT hr = ID3D11Device1_CreateDepthStencilView(pD->pDev1, r->pResource, &d, &v->pDSV);
    if (FAILED(hr)) { TR_LOG("CreateDSV: 0x%08lx", hr); v->pDSV = NULL; tritonSetCreateError(pD, hr); return; }
    tritonResourceLinkView(r, &v->link, TRITON_VIEW_DSV);
}

void APIENTRY
tritonDestroyDSV(D3D10DDI_HDEVICE hDev, D3D10DDI_HDEPTHSTENCILVIEW hDSV)
{
    PTRITON_DSVIEW v = (PTRITON_DSVIEW)(hDSV.pDrvPrivate);
    if (!v) return;
    tritonViewUnlink(&v->link);
    if (v->pDSV) { ID3D11DepthStencilView_Release(v->pDSV); v->pDSV = NULL; }
    v->pResource = NULL;
}

/* ---------- UnorderedAccessView ---------- */

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateUAVSize, TRITON_UAVIEW,
                     const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *pArgs)

/* The 11.x create args are a prefix of the WDDM 2.0 ones: the Tex2D member
 * only gains a trailing PlaneSlice, and no other member reaches past it, so
 * the bytes before PlaneSlice carry every 11.x field. */
#define TRITON_UAV_ARGS11_BYTES offsetof(D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW, Tex2D.PlaneSlice)
_Static_assert(offsetof(D3D11DDIARG_CREATEUNORDEREDACCESSVIEW, Tex2D) == offsetof(D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW, Tex2D) &&
               offsetof(D3D11DDIARG_CREATEUNORDEREDACCESSVIEW, Tex2D) + sizeof(((D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *)0)->Tex2D) ==
                   TRITON_UAV_ARGS11_BYTES &&
               TRITON_UAV_ARGS11_BYTES <= sizeof(D3D11DDIARG_CREATEUNORDEREDACCESSVIEW),
               "UAV create args are not prefix-compatible");
_Static_assert(sizeof(((D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *)0)->Buffer) <= sizeof(((D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *)0)->Tex2D),
               "UAV create arg union outgrew Tex2D");
_Static_assert(sizeof(((D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *)0)->Tex1D) <= sizeof(((D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *)0)->Tex2D),
               "UAV create arg union outgrew Tex2D");
_Static_assert(sizeof(((D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *)0)->Tex3D) <= sizeof(((D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *)0)->Tex2D),
               "UAV create arg union outgrew Tex2D");

static BOOL
tritonBuildUavDesc(const TRITON_RESOURCE *r, const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *a,
                   D3D11_UNORDERED_ACCESS_VIEW_DESC1 *d)
{
    memset(d, 0, sizeof(*d));
    d->Format = a->Format;
    const BOOL fIsArray = (r->ArraySize > 1);
    switch (a->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
    case D3D11DDIRESOURCE_BUFFEREX:
        d->ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        d->Buffer.FirstElement = a->Buffer.FirstElement;
        d->Buffer.NumElements  = a->Buffer.NumElements;
        d->Buffer.Flags        = a->Buffer.Flags;
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
        if (fIsArray) {
            d->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
            d->Texture1DArray.MipSlice        = a->Tex1D.MipSlice;
            d->Texture1DArray.FirstArraySlice = a->Tex1D.FirstArraySlice;
            d->Texture1DArray.ArraySize       = a->Tex1D.ArraySize;
        } else {
            d->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
            d->Texture1D.MipSlice = a->Tex1D.MipSlice;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        if (fIsArray) {
            d->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            d->Texture2DArray.MipSlice        = a->Tex2D.MipSlice;
            d->Texture2DArray.FirstArraySlice = a->Tex2D.FirstArraySlice;
            d->Texture2DArray.ArraySize       = a->Tex2D.ArraySize;
            d->Texture2DArray.PlaneSlice      = a->Tex2D.PlaneSlice;
        } else {
            d->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            d->Texture2D.MipSlice    = a->Tex2D.MipSlice;
            d->Texture2D.PlaneSlice  = a->Tex2D.PlaneSlice;
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE3D:
        d->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        d->Texture3D.MipSlice    = a->Tex3D.MipSlice;
        d->Texture3D.FirstWSlice = a->Tex3D.FirstW;
        d->Texture3D.WSize       = a->Tex3D.WSize;
        break;
    default:
        TR_LOG("CreateUAV: unknown dimension %d", a->ResourceDimension);
        return FALSE;
    }
    return TRUE;
}

/* Views are created through the 11.3 *View1 call when the host exposes it,
 * so PlaneSlice reaches it; otherwise through the base call, which takes the
 * DESC1 with its trailing PlaneSlice fields dropped (DESC is a prefix of
 * DESC1 member by member). */
static void
tritonCreateUavCommon(PTRITON_DEVICE pD, TRITON_UAVIEW *v, const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *a)
{
    PTRITON_RESOURCE r = (PTRITON_RESOURCE)(a->hDrvResource.pDrvPrivate);
    if (!pD || !v || !r || !r->pResource) { tritonSetCreateError(pD, E_INVALIDARG); return; }
    v->pResource = r;
    v->pUAV = NULL;

    D3D11_UNORDERED_ACCESS_VIEW_DESC1 d;
    if (!tritonBuildUavDesc(r, a, &d)) {
        tritonSetCreateError(pD, E_INVALIDARG);
        return;
    }
    HRESULT hr;
    if (pD->pDev3) {
        ID3D11UnorderedAccessView1 *p1 = NULL;
        v->fDesc1 = TRUE;
        v->Desc1  = d;
        hr = ID3D11Device3_CreateUnorderedAccessView1(pD->pDev3, r->pResource, &d, &p1);
        v->pUAV = (ID3D11UnorderedAccessView *)p1;
    } else {
        v->fDesc1 = FALSE;
        memcpy(&v->Desc, &d, sizeof(v->Desc));
        hr = ID3D11Device1_CreateUnorderedAccessView(pD->pDev1, r->pResource, &v->Desc, &v->pUAV);
    }
    if (FAILED(hr) || !v->pUAV) {
        TR_LOG("CreateUAV: 0x%08lx", hr);
        v->pUAV = NULL;
        tritonSetCreateError(pD, hr);
        return;
    }
    tritonResourceLinkView(r, &v->link, TRITON_VIEW_UAV);
}

void APIENTRY
tritonCreateUAV(D3D10DDI_HDEVICE hDevice, const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *pArgs,
                D3D11DDI_HUNORDEREDACCESSVIEW hView, D3D11DDI_HRTUNORDEREDACCESSVIEW hRTView)
{
    (void)hRTView;
    D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW a;
    memset(&a, 0, sizeof(a));
    memcpy(&a, pArgs, TRITON_UAV_ARGS11_BYTES);
    tritonCreateUavCommon((PTRITON_DEVICE)hDevice.pDrvPrivate,
                          (TRITON_UAVIEW *)hView.pDrvPrivate, &a);
}

void APIENTRY
tritonCreateUAV_WDDM2_0(D3D10DDI_HDEVICE hDevice, const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *pArgs,
                        D3D11DDI_HUNORDEREDACCESSVIEW hView, D3D11DDI_HRTUNORDEREDACCESSVIEW hRTView)
{
    (void)hRTView;
    tritonCreateUavCommon((PTRITON_DEVICE)hDevice.pDrvPrivate,
                          (TRITON_UAVIEW *)hView.pDrvPrivate, pArgs);
}

void APIENTRY
tritonDestroyUAV(D3D10DDI_HDEVICE hDev, D3D11DDI_HUNORDEREDACCESSVIEW hUAV)
{
    PTRITON_UAVIEW v = (PTRITON_UAVIEW)(hUAV.pDrvPrivate);
    if (!v) return;
    tritonViewUnlink(&v->link);
    if (v->pUAV) { ID3D11UnorderedAccessView_Release(v->pUAV); v->pUAV = NULL; }
    v->pResource = NULL;
}

void APIENTRY
tritonClearUAVUint(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hUAV,
                   const UINT Values[4])
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_UAVIEW v  = (PTRITON_UAVIEW)(hUAV.pDrvPrivate);
    if (!pD || !v || !v->pUAV) return;
    ID3D11DeviceContext1_ClearUnorderedAccessViewUint(pD->pCtx1, v->pUAV, Values);
}

void APIENTRY
tritonClearUAVFloat(D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW hUAV,
                    const FLOAT Values[4])
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_UAVIEW v  = (PTRITON_UAVIEW)(hUAV.pDrvPrivate);
    if (!pD || !v || !v->pUAV) return;
    ID3D11DeviceContext1_ClearUnorderedAccessViewFloat(pD->pCtx1, v->pUAV, Values);
}

void APIENTRY
tritonCopyStructureCount(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDstBuffer,
                         UINT DstAlignedByteOffset, D3D11DDI_HUNORDEREDACCESSVIEW hSrcUAV)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE d  = (PTRITON_RESOURCE)(hDstBuffer.pDrvPrivate);
    PTRITON_UAVIEW   v  = (PTRITON_UAVIEW)(hSrcUAV.pDrvPrivate);
    if (!pD || !d || !v || !d->pResource || !v->pUAV) return;
    ID3D11DeviceContext1_CopyStructureCount(
        pD->pCtx1, (ID3D11Buffer *)d->pResource, DstAlignedByteOffset, v->pUAV);
}

/* ---------- Per-stage SRV bind ---------- */

#define TR_SRV_SETTER(stage, ctx_method)                                                 \
void APIENTRY                                                                 \
triton ## stage ## SetShaderResources(D3D10DDI_HDEVICE hDevice, UINT Start, UINT N,      \
                                      const D3D10DDI_HSHADERRESOURCEVIEW *ph)            \
{                                                                                        \
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);                           \
    if (!pD) return;                                                                     \
    if (Start > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ||                          \
        N > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - Start) return;                \
    ID3D11ShaderResourceView *a[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};      \
    for (UINT i = 0; i < N; ++i) {                                                       \
        PTRITON_SRVIEW v = ph                                                            \
            ? (PTRITON_SRVIEW)(ph[i].pDrvPrivate)                                        \
            : NULL;                                                                      \
        a[i] = v ? v->pSRV : NULL;                                                       \
    }                                                                                    \
    ID3D11DeviceContext1_##ctx_method(pD->pCtx1, Start, N, a);                           \
}

TR_SRV_SETTER(Vs, VSSetShaderResources)
TR_SRV_SETTER(Ps, PSSetShaderResources)
TR_SRV_SETTER(Gs, GSSetShaderResources)
TR_SRV_SETTER(Hs, HSSetShaderResources)
TR_SRV_SETTER(Ds, DSSetShaderResources)
TR_SRV_SETTER(Cs, CSSetShaderResources)

#undef TR_SRV_SETTER

void APIENTRY
tritonCsSetUAVs(D3D10DDI_HDEVICE hDevice, UINT Start, UINT N,
                const D3D11DDI_HUNORDEREDACCESSVIEW *ph, const UINT *pInitial)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return;
    if (N > D3D11_1_UAV_SLOT_COUNT || Start >= D3D11_1_UAV_SLOT_COUNT ||
        N + Start > D3D11_1_UAV_SLOT_COUNT)
        return;
    ID3D11UnorderedAccessView *a[D3D11_1_UAV_SLOT_COUNT] = {};
    for (UINT i = 0; i < N; ++i) {
        PTRITON_UAVIEW v = ph
            ? (PTRITON_UAVIEW)(ph[i].pDrvPrivate)
            : NULL;
        a[i] = v ? v->pUAV : NULL;
    }
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(pD->pCtx1, Start, N, a, pInitial);
}

void APIENTRY
tritonGenerateMips(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADERRESOURCEVIEW hSRV)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SRVIEW v  = (PTRITON_SRVIEW)(hSRV.pDrvPrivate);
    if (!pD || !v || !v->pSRV) return;
    ID3D11DeviceContext1_GenerateMips(pD->pCtx1, v->pSRV);
}

/* ---------- WDDM 2.0 view sizes (creates are above) ---------- */

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateSRVSize_WDDM2_0, TRITON_SRVIEW,
                     const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *pArgs)

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateRenderTargetViewSize_WDDM2_0, TRITON_RTVIEW,
                     const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *pArgs)

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateUAVSize_WDDM2_0, TRITON_UAVIEW,
                     const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *pArgs)

/* WDDM 2.0 Flush(ContextType, FlushFlags).
 *
 * ContextType is the DDI's D3DWDDM2_0DDI_CONTEXTTYPE_FLAG bit flag, which
 * is NOT value-equal to D3D11_CONTEXT_TYPE at COPY/VIDEO (DDI COPY=4 would
 * cast to VIDEO; DDI VIDEO=8 is out of range); convert via
 * tritonContextFlagToEnum rather than cast. Falls back to the 11.1 flush
 * (ignores ContextType) if Neptune doesn't expose Context3. */
BOOL APIENTRY
tritonFlush_WDDM2_0(D3D10DDI_HDEVICE hDevice, UINT ContextType, UINT FlushFlags)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD) return FALSE;
    if (pD->pCtx3) {
        ID3D11DeviceContext3_Flush1(pD->pCtx3, tritonContextFlagToEnum(ContextType), NULL);
        return TRUE;
    }
    return tritonFlush11_1(hDevice, FlushFlags);
}
