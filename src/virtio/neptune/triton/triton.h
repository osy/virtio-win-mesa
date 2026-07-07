/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * State structs and internal declarations shared across the Triton DDI
 * translation units. Each struct sits behind the corresponding
 * D3D10DDI_H* handle's pDrvPrivate; the runtime allocates storage via
 * CalcPrivate*Size and we populate it in the matching pfnCreate*.
 */

#ifndef TRITON_H_INCLUDED
#define TRITON_H_INCLUDED

/* The triton .c files use C-style COM method invocations (e.g.
 * ID3D11Device1_Release(p)); the COBJMACROS define switches d3d*.h
 * into emitting those wrapper macros. */
#define COBJMACROS

#include <windows.h>
/* d3dkmthk.h declares DDI signatures whose return type is NTSTATUS;
 * winternl.h is the standard user-mode source for that typedef. */
#include <winternl.h>
#include <d3dkmthk.h>

/* Suppress d3d10effect.h: Triton doesn't use the D3D10 effects runtime,
 * and the SDK header pulls in slot-count macros lazily in a way that
 * breaks under our include order. */
#define __D3D10EFFECT_H__

#include <d3d10umddi.h>
#include <d3d11_3.h>
/* DXGI DDI base-callback / present types (PFNDDXGIDDI_PRESENTCB,
 * DXGIDDICB_PRESENT). tritonDxgi.c includes this after triton.h, proving
 * the d3d10umddi.h -> dxgiddi.h order resolves. */
#include <dxgiddi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TRITON_ADAPTER {
    D3D10DDI_HRTADAPTER          hRTAdapter;
    UINT                         uIfVersion;     /* D3D11_*_DDI_INTERFACE_VERSION */
    UINT                         uRtVersion;
    D3DDDI_ADAPTERCALLBACKS      RtCallbacks;
} TRITON_ADAPTER, *PTRITON_ADAPTER;

struct TRITON_SHADER;
struct TRITON_ELEMENTLAYOUT;

typedef struct TRITON_DEVICE {
    D3D10DDI_HRTDEVICE              hRTDevice;
    UINT                            uIfVersion;
    UINT                            uRtVersion;
    PTRITON_ADAPTER                 pAdapter;
    D3DDDI_DEVICECALLBACKS          KTCallbacks;

    /* pDev1/pCtx1 come from the Neptune factory. pDev2/pDev3/pCtx2/pCtx3
     * are QI'd in tritonCreateDevice and may be NULL when the host stack
     * does not expose the newer interfaces. WDDM 1.3+ handlers check for
     * NULL and fall back to TR_STUB. */
    ID3D11Device1                  *pDev1;
    ID3D11Device2                  *pDev2;
    ID3D11Device3                  *pDev3;
    ID3D11DeviceContext1           *pCtx1;
    ID3D11DeviceContext2           *pCtx2;
    ID3D11DeviceContext3           *pCtx3;
    D3D_FEATURE_LEVEL               FeatureLevel;

    /* Currently-bound VS and element layout. Used to lazily create the
     * ID3D11InputLayout the first time both are known. Weak refs: the
     * owning TRITON_SHADER / TRITON_ELEMENTLAYOUT may be destroyed while
     * still referenced here; we clear from those destroy paths. */
    struct TRITON_SHADER           *pCurrentVS;
    struct TRITON_ELEMENTLAYOUT    *pCurrentLayout;

    /* Monotonic per-device shader-instance counter. Each shader gets a
     * unique cookie at create; the input-layout cache keys on it so a
     * freed VS bytecode pointer reused at the same heap address cannot
     * alias a layout built against the old VS's input signature. */
    UINT64                          nextShaderCookie;

    /* Runtime callback handle + table. The DDI is mostly VOID-returning;
     * to signal asynchronous failures we route through pfnSetErrorCb. */
    D3D10DDI_HRTCORELAYER                            hRTCoreLayer;
    const D3D11DDI_CORELAYER_DEVICECALLBACKS        *pUMCallbacks;

    /* Kernel context for present submission, created lazily via
     * KTCallbacks.pfnCreateContextCb on the first primary-create or present
     * (tritonPresentEnsureKernelContext). pfnPresentCb (recorded from
     * DXGIBaseDDI.pDXGIBaseCallbacks) consumes it to reach the KMD's
     * DxgkDdiPresent / DxgkDdiSetVidPnSourceAddress. NULL until created:
     * present is then unavailable but rendering still works. */
    HANDLE                                           hKMContext;
    PFNDDXGIDDI_PRESENTCB                            pfnPresentCb;

    /* Set once tritonPresentEnsureRuntimeCtx has issued VIOGPU_CTX_INIT
     * for this device, giving its KMD context a valid virtio context
     * for RESOURCE_CREATE_BLOB / CTX_ATTACH_RESOURCE. */
    BOOL                                             RuntimeCtxInited;
} TRITON_DEVICE, *PTRITON_DEVICE;

/* dxvk-native vendor MiscFlags bit (DXVK_D3D11_RESOURCE_MISC_LINEAR_EXPORT
 * in dxvk_shared_resource.h): with MISC_SHARED, forces the dmabuf export to
 * DRM_FORMAT_MOD_LINEAR so scanout consumers without modifier plumbing can
 * import it.  Value is ABI with the host dxvk build. */
#define TRITON_D3D11_MISC_LINEAR_EXPORT 0x40000000u

static inline void tritonSetError(PTRITON_DEVICE pD, HRESULT hr)
{
    if (pD && pD->pUMCallbacks && pD->pUMCallbacks->pfnSetErrorCb)
        pD->pUMCallbacks->pfnSetErrorCb(pD->hRTCoreLayer, hr);
}

/* Map the DDI's D3DWDDM2_0DDI_CONTEXTTYPE_FLAG bit flag to the sequential
 * D3D11_CONTEXT_TYPE enum (they diverge at COPY/VIDEO). Defined in
 * tritonQuery.c; shared with the WDDM 2.0 Flush path in tritonView.c. */
D3D11_CONTEXT_TYPE tritonContextFlagToEnum(UINT flag);

/* Intrusive per-resource view list.  RotateResourceIdentities must
 * recreate every host view wrapping a rotated resource, so each view
 * links itself onto its owning resource at create and unlinks at
 * destroy. */
typedef enum TRITON_VIEW_KIND {
    TRITON_VIEW_RTV = 0,
    TRITON_VIEW_SRV,
    TRITON_VIEW_DSV,
    TRITON_VIEW_UAV,
} TRITON_VIEW_KIND;

typedef struct TRITON_VIEWLINK {
    struct TRITON_VIEWLINK  *pNext;
    struct TRITON_VIEWLINK **ppPrev;  /* NULL when not linked */
    TRITON_VIEW_KIND         kind;
} TRITON_VIEWLINK;

typedef struct TRITON_RESOURCE {
    D3D10DDI_HRTRESOURCE   hRTResource;
    D3D10DDIRESOURCE_TYPE  dim;
    DXGI_FORMAT            Format;
    UINT                   Width;
    UINT                   Height;
    UINT                   Depth;
    UINT                   MipLevels;
    UINT                   ArraySize;
    UINT                   BindFlags;
    UINT                   MiscFlags;
    UINT                   MapFlags;     /* D3D10_DDI_CPU_ACCESS_* */
    UINT                   Usage;        /* D3D10_DDI_USAGE_* */
    DXGI_SAMPLE_DESC       SampleDesc;
    UINT                   ByteStride;

    /* TRUE when this resource is a display primary (BIND_PRESENT with
     * pPrimaryDesc): its host texture is a linear dmabuf export and its
     * KM allocation is a flippable scanout primary the KMD scans out
     * via SET_SCANOUT_BLOB. */
    BOOL                   IsPresentable;

    /* KM allocation backing this resource's virtio-gpu blob (shared
     * textures and primaries; 0 otherwise).  Owned by the creator;
     * consumers leave it 0 (the runtime owns their opened allocation). */
    D3DKMT_HANDLE          hKMAllocation;

    /* TRUE when the resource is shared (creator) or opened (consumer). */
    BOOL                   IsShared;

    /* Consumer-side import rig: WDDM handles of the transport-device
     * TYPE_IMPORT allocation that keeps the blob attached to this
     * process's transport context (0 when not a consumer). */
    D3DKMT_HANDLE          hImportAlloc;
    D3DKMT_HANDLE          hImportResKmt;

    ID3D11Resource        *pResource;

    /* Head of the intrusive list of live views wrapping this resource. */
    TRITON_VIEWLINK       *pViewList;
} TRITON_RESOURCE, *PTRITON_RESOURCE;

typedef struct TRITON_RTVIEW {
    D3D10DDI_HRTRENDERTARGETVIEW   hRTRenderTargetView;
    PTRITON_RESOURCE                pResource;
    ID3D11RenderTargetView         *pRTV;
    TRITON_VIEWLINK                 link;
} TRITON_RTVIEW, *PTRITON_RTVIEW;

typedef struct TRITON_SRVIEW {
    D3D10DDI_HRTSHADERRESOURCEVIEW  hRT;
    PTRITON_RESOURCE                pResource;
    ID3D11ShaderResourceView       *pSRV;
    TRITON_VIEWLINK                 link;
} TRITON_SRVIEW, *PTRITON_SRVIEW;

typedef struct TRITON_DSVIEW {
    D3D10DDI_HRTDEPTHSTENCILVIEW    hRT;
    PTRITON_RESOURCE                pResource;
    ID3D11DepthStencilView         *pDSV;
    TRITON_VIEWLINK                 link;
} TRITON_DSVIEW, *PTRITON_DSVIEW;

typedef struct TRITON_UAVIEW {
    D3D11DDI_HRTUNORDEREDACCESSVIEW hRT;
    PTRITON_RESOURCE                pResource;
    ID3D11UnorderedAccessView      *pUAV;
    TRITON_VIEWLINK                 link;
} TRITON_UAVIEW, *PTRITON_UAVIEW;

typedef struct TRITON_BLENDSTATE {
    D3D10DDI_HRTBLENDSTATE   hRT;
    ID3D11BlendState1       *pState;
} TRITON_BLENDSTATE, *PTRITON_BLENDSTATE;

typedef struct TRITON_DEPTHSTENCILSTATE {
    D3D10DDI_HRTDEPTHSTENCILSTATE   hRT;
    ID3D11DepthStencilState        *pState;
} TRITON_DEPTHSTENCILSTATE, *PTRITON_DEPTHSTENCILSTATE;

typedef struct TRITON_RASTERIZERSTATE {
    D3D10DDI_HRTRASTERIZERSTATE   hRT;
    ID3D11RasterizerState1       *pState;
} TRITON_RASTERIZERSTATE, *PTRITON_RASTERIZERSTATE;

typedef struct TRITON_SAMPLER {
    D3D10DDI_HRTSAMPLER   hRT;
    ID3D11SamplerState   *pState;
} TRITON_SAMPLER, *PTRITON_SAMPLER;

typedef struct TRITON_ELEMENTLAYOUT {
    D3D10DDI_HRTELEMENTLAYOUT       hRT;
    UINT                            NumElements;
    D3D10DDIARG_INPUT_ELEMENT_DESC *pElements;  /* heap copy (driver-owned) */
    ID3D11InputLayout              *pLayout;    /* NULL until first VS is known */
    UINT64                          LayoutVsCookie; /* cookie of the VS pLayout was built against; 0 = none */
} TRITON_ELEMENTLAYOUT, *PTRITON_ELEMENTLAYOUT;

typedef enum TRITON_SHADER_KIND {
    TRITON_SHADER_VS = 0,
    TRITON_SHADER_PS,
    TRITON_SHADER_GS,
    TRITON_SHADER_HS,
    TRITON_SHADER_DS,
    TRITON_SHADER_CS,
} TRITON_SHADER_KIND;

typedef struct TRITON_QUERY {
    D3D10DDI_HRTQUERY  hRT;
    D3D10DDI_QUERY     ddiKind;
    ID3D11Query       *pQuery;
} TRITON_QUERY, *PTRITON_QUERY;

typedef struct TRITON_SHADER {
    D3D10DDI_HRTSHADER     hRT;
    TRITON_SHADER_KIND     kind;
    void                  *pBytecode;   /* heap copy; freed on destroy */
    SIZE_T                 cbBytecode;
    UINT64                 cookie;       /* unique instance id; input-layout cache key */
    union {
        ID3D11DeviceChild       *pDeviceChild;
        ID3D11VertexShader      *pVS;
        ID3D11PixelShader       *pPS;
        ID3D11GeometryShader    *pGS;
        ID3D11HullShader        *pHS;
        ID3D11DomainShader      *pDS;
        ID3D11ComputeShader     *pCS;
    } u;
} TRITON_SHADER, *PTRITON_SHADER;

/* View-list plumbing + rotation support (tritonView.c). */
void tritonResourceLinkView(PTRITON_RESOURCE r, TRITON_VIEWLINK *l,
                            TRITON_VIEW_KIND kind);
void tritonViewUnlink(TRITON_VIEWLINK *l);
HRESULT tritonResourceRecreateViews(PTRITON_DEVICE pD, PTRITON_RESOURCE r);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TRITON_H_INCLUDED */
