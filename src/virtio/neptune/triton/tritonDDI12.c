/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * OpenAdapter12: the D3D12 DDI front-end.
 *
 * Adapter-scope caps are answered here, before any device exists; the
 * device-scope tables live in the per-object TUs.  GetCaps zero-fills
 * every cap type it does not answer explicitly, so a type whose zero
 * value is a lie -- or which the runtime passes in/out -- needs a case
 * of its own.
 */

#include "triton12.h"
#include "triton_log.h"
#include "tritonSharedBridge.h"
#include "tritonHostCaps.h"
#include "npt_env.h"

/* npt_device.h can't be included here: its vendored protocol DirectX
 * types collide with the SDK headers this TU already pulls. */
#include <stdbool.h>
extern bool npt_d3d12_from_ddi;


HRESULT
npt_d3d12_create_device_internal(IUnknown *pAdapter,
                                 D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                 REFIID riid,
                                 void **ppDevice);

/* ---------- adapter funcs ---------- */

static SIZE_T APIENTRY
triton12CalcPrivateDeviceSize(D3D12DDI_HADAPTER hAdapter,
                              const D3D12DDIARG_CALCPRIVATEDEVICESIZE *pArgs)
{
    (void)hAdapter;
    TR_LOG("12.CalcPrivateDeviceSize: Interface=0x%08x Version=0x%08x Flags=0x%x",
           pArgs->Interface, pArgs->Version, (unsigned)pArgs->Flags);
    return sizeof(TRITON12_DEVICE);
}

static HRESULT APIENTRY
triton12CreateDevice(D3D12DDI_HADAPTER hAdapter,
                     const D3D12DDIARG_CREATEDEVICE_0003 *pArgs)
{
    PTRITON12_ADAPTER pAdapter = (PTRITON12_ADAPTER)hAdapter.pDrvPrivate;
    PTRITON12_DEVICE p = (PTRITON12_DEVICE)pArgs->hDrvDevice.pDrvPrivate;

    TR_LOG("12.CreateDevice: Interface=0x%08x Version=0x%08x Flags=0x%x",
           pArgs->Interface, pArgs->Version, (unsigned)pArgs->Flags);
    if (!pAdapter || !p)
        return E_INVALIDARG;

    memset(p, 0, sizeof(*p));
    p->pAdapter     = pAdapter;
    p->hRTDevice    = pArgs->hRTDevice;
    p->KTCallbacks  = *pArgs->pKTCallbacks;
    p->pUMCallbacks = pArgs->p12UMCallbacks_0022;

    /* The caps snapshot (GetCaps) already stood up this exact device;
     * adopt it rather than create a second host device.  A later
     * CreateDevice on the same adapter creates its own. */
    ID3D12Device *dev = (ID3D12Device *)InterlockedExchangePointer(
        (PVOID volatile *)&pAdapter->pCapsDev, NULL);
    if (dev) {
        TR_LOG("12.CreateDevice: adopting the caps-snapshot device (%p)",
               (void *)dev);
    } else {
        HRESULT hr = npt_d3d12_create_device_internal(
            NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
        if (FAILED(hr)) {
            TR_LOG("12.CreateDevice: inner device create failed 0x%08lx",
                   (unsigned long)hr);
            return hr;
        }
        TR_LOG("12.CreateDevice: inner Neptune ID3D12Device up (%p)", (void *)dev);
    }
    p->pDev = dev;

    /* Host tiled-resource support, which picks the reserved-resource
     * path in tritonResource12.c: a nonzero tier means the host honours
     * CreateReservedResource + UpdateTileMappings with real sparse
     * backing. */
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS opts;
        memset(&opts, 0, sizeof(opts));
        p->HostTiledTier = 0;
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(
                dev, D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))))
            p->HostTiledTier = (UINT)opts.TiledResourcesTier;
        TR_LOG("12.CreateDevice: host TiledResourcesTier=%u -> reserved resources %s",
               p->HostTiledTier,
               p->HostTiledTier ? "FORWARDED (sparse host backing)"
                                : "committed-backing shim");
    }

    /* Queue registry lock, for cross-queue submission ordering.  Initialised
     * here rather than lazily on first queue create: CreateCommandQueue is
     * free-threaded, so a lazy init would race two queues into two locks. */
    InitializeCriticalSection(&p->QueueLock);
    p->QueueLockInit = TRUE;

    /* Device-funcs tables are not filled here: the runtime requests each
     * one separately through pfnFillDDITable. */
    return S_OK;
}

static VOID APIENTRY
triton12DestroyDevice(D3D12DDI_HDEVICE hDevice)
{
    PTRITON12_DEVICE p = (PTRITON12_DEVICE)hDevice.pDrvPrivate;
    TR_LOG("12.DestroyDevice");
    if (!p)
        return;
    if (p->pDev) {
        ID3D12Device_Release(p->pDev);
        p->pDev = NULL;
    }
    if (p->QueueLockInit) {
        p->QueueLockInit = FALSE;
        p->QueueCount = 0;
        DeleteCriticalSection(&p->QueueLock);
    }
    p->pAdapter = NULL;
}

static HRESULT APIENTRY
triton12CloseAdapter(D3D12DDI_HADAPTER hAdapter)
{
    PTRITON12_ADAPTER pAdapter = (PTRITON12_ADAPTER)hAdapter.pDrvPrivate;
    TR_LOG("12.CloseAdapter");
    if (pAdapter) {
        if (pAdapter->pCapsDev)
            ID3D12Device_Release(pAdapter->pCapsDev);
        if (pAdapter->pHostCaps12)
            HeapFree(GetProcessHeap(), 0, pAdapter->pHostCaps12);
        HeapFree(GetProcessHeap(), 0, pAdapter);
    }
    return S_OK;
}

static HRESULT APIENTRY
triton12GetSupportedVersions(D3D12DDI_HADAPTER hAdapter,
                             UINT32 *puEntries,
                             UINT64 *pSupportedDDIInterfaceVersions)
{
    (void)hAdapter;
    /* Offer _0022 alone: it is the only revision whose tables, callback
     * struct and cap types this driver implements.  A lower revision is
     * not a weaker promise but a different one -- the runtime would hand
     * over smaller tables, which pfnFillDDITable refuses to populate
     * because it requires the _0022 sizes, leaving the runtime to
     * dispatch through stubs. */
    static const UINT64 kSupportedVersions[] = { D3D12DDI_SUPPORTED_0022 };
    const UINT32 kCount =
        sizeof(kSupportedVersions) / sizeof(kSupportedVersions[0]);

    if (!puEntries)
        return E_INVALIDARG;

    if (!pSupportedDDIInterfaceVersions) {
        TR_LOG("12.GetSupportedVersions: size query -> %u", kCount);
        *puEntries = kCount;
        return S_OK;
    }

    const UINT32 toCopy = (*puEntries < kCount) ? *puEntries : kCount;
    for (UINT32 i = 0; i < toCopy; ++i) {
        pSupportedDDIInterfaceVersions[i] = kSupportedVersions[i];
        TR_LOG("12.GetSupportedVersions: offering 0x%016llx",
               (unsigned long long)kSupportedVersions[i]);
    }
    *puEntries = toCopy;
    return S_OK;
}

/* ---------- adapter-scope host caps ---------- */

static BOOL CALLBACK
triton12HostCapsInit(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)ctx;
    PTRITON12_ADAPTER pAdapter = (PTRITON12_ADAPTER)param;
    struct triton_host_caps12 *s = (struct triton_host_caps12 *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
    if (!s)
        return TRUE;
    ID3D12Device *dev = NULL;
    tritonHostCaps12Snapshot(s, &dev);
    pAdapter->pCapsDev = dev;
    pAdapter->pHostCaps12 = s;
    return TRUE;
}

static const struct triton_host_caps12 *
triton12HostCaps(PTRITON12_ADAPTER pAdapter)
{
    static const struct triton_host_caps12 kNone;
    if (!pAdapter)
        return &kNone;
    InitOnceExecuteOnce(&pAdapter->HostCapsOnce, triton12HostCapsInit,
                        pAdapter, NULL);
    return pAdapter->pHostCaps12 ? pAdapter->pHostCaps12 : &kNone;
}

#define HC12(bit) ((hc->have & TRITON_HC12_##bit) != 0)
#define CAP12_HOST(name, lv, bit, hv) \
    TRITON_CAP_HOST("d3d12", name, lv, HC12(bit), hv)
#define CAP12_FIXED(name, lv, bit, hv, v, why) \
    TRITON_CAP_FIXED("d3d12", name, lv, HC12(bit), hv, v, why)
#define CAP12_TODO(name, lv, bit, hv, v, why) \
    TRITON_CAP_TODO("d3d12", name, lv, HC12(bit), hv, v, why)

static HRESULT APIENTRY
triton12GetCaps(D3D12DDI_HADAPTER hAdapter, const D3D12DDIARG_GETCAPS *pArgs)
{
    PTRITON12_ADAPTER pAdapter = (PTRITON12_ADAPTER)hAdapter.pDrvPrivate;
    const UINT32 hostCaps = pAdapter ? pAdapter->HostCaps : 0;
    TR_LOG("12.GetCaps: Type=%d DataSize=%u pInfo=%p",
           (int)pArgs->Type, pArgs->DataSize, pArgs->pInfo);
    if (!pArgs->pData || !pArgs->DataSize)
        return S_OK;

    const struct triton_host_caps12 *hc = triton12HostCaps(pAdapter);

    /* Pipeline level.  FL 12_1 when the host takes DXIL: the label is
     * what apps check before they start, and ValidateCapsForFeatureLevel
     * requires TiledResourcesTier >= 2, TypedUAVLoadAdditionalFormats and
     * ConservativeRasterizationTier >= 1 behind it -- see D3D12_OPTIONS
     * for which of those are host-backed and which are label-only.  Kill
     * switch: host NPT_CAPSET_CAPS=0x1 clears the DXIL bit and everything
     * (SM6 + FL12 + tiled) reverts in one boot. */
    const UINT pipelineLevel = (hostCaps & TRITON_HOSTCAP_DXIL)
                                   ? (UINT)D3D12DDI_3DPIPELINELEVEL_12_1
                                   : (UINT)D3D12DDI_3DPIPELINELEVEL_11_1;

    if ((int)pArgs->Type == 1074 /* D3D12DDICAPS_TYPE_0081_3DPIPELINESUPPORT1 */) {
        /* In/out struct, exempt from the zero-fill default: field 0 is
         * the runtime's input (HighestRuntimeSupportedFeatureLevel) and
         * field 1 our answer, where zero reads as "no pipeline". */
        if (pArgs->DataSize >= 2 * sizeof(UINT)) {
            UINT *pLevels = (UINT *)pArgs->pData;
            TR_LOG("12.GetCaps 3DPIPELINESUPPORT1: runtime highest=%u", pLevels[0]);
            CAP12_FIXED("3DPIPELINESUPPORT1.Level", pLevels[1],
                        SHADER_MODEL, hc->shaderModel.HighestShaderModel,
                        pipelineLevel,
                        "FL label from the host DXIL capset bit; host value is its shader model");
        }
        return S_OK;
    }

    if ((int)pArgs->Type == D3D12DDICAPS_TYPE_0011_SHADER_MODELS) {
        /* In/out pointer pair, exempt from the zero-fill default: the
         * runtime supplies the pointers. */
        D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 *pData =
            (D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 *)pArgs->pData;
        /* DXBC 5.1 is mandatory -- a list without it is a validation
         * reject.  SM 6.x is DXIL: advertised only when the host backend
         * consumes DXIL containers (D3DMetal yes, DXMT no).  Advertising
         * it makes the runtime treat this driver as a DXIL consumer, so
         * DXIL apps hand their containers through pfnCreateShader
         * (tritonPipeline12.c detects the container magic).  The range
         * follows the host's SHADER_MODEL.HighestShaderModel, capped at
         * 6.6: 6.7+ carry cap types (SHADER_MODEL_6_8_OPTIONS_0110) this
         * DDI revision cannot answer. */
        UINT models[16];
        UINT cModels = 0;
        models[cModels++] = 0x00050015; /* D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011 */
        UINT highest = 0;
        if (hostCaps & TRITON_HOSTCAP_DXIL) {
            UINT hostHighest = HC12(SHADER_MODEL)
                                   ? (UINT)hc->shaderModel.HighestShaderModel
                                   : 0x66;
            CAP12_TODO("SHADER_MODELS.Highest", highest, SHADER_MODEL,
                       hostHighest, hostHighest > 0x66 ? 0x66 : hostHighest,
                       "SM 6.7+ needs the _0110 cap types: DDI revision bump");
            /* Release-bit encoding: 0x0006MMm5 for 6.M (d3d12umddi.h). */
            for (UINT minor = 0; highest >= 0x60 && minor <= (highest & 0xf); minor++)
                models[cModels++] = 0x00060005 | (minor << 4);
        } else {
            CAP12_FIXED("SHADER_MODELS.Highest", highest, SHADER_MODEL,
                        hc->shaderModel.HighestShaderModel, 0x51,
                        "host backend has no DXIL front end (capset)");
        }
        if (!pData->pNumShaderModelsSupported)
            return S_OK;
        if (!pData->pShaderModelsSupported) {
            *pData->pNumShaderModelsSupported = cModels;
        } else {
            UINT n = *pData->pNumShaderModelsSupported;
            if (n > cModels)
                n = cModels;
            for (UINT i = 0; i < n; i++)
                pData->pShaderModelsSupported[i] =
                    (D3D12DDI_SHADER_MODEL)models[i];
            *pData->pNumShaderModelsSupported = n;
        }
        TR_LOG("12.GetCaps SHADER_MODELS: %u models (highest 0x%x, dxil=%d)",
               cModels, highest, (hostCaps & TRITON_HOSTCAP_DXIL) ? 1 : 0);
        return S_OK;
    }

    ZeroMemory(pArgs->pData, pArgs->DataSize);

    switch ((int)pArgs->Type) {
    case D3D12DDICAPS_TYPE_3DPIPELINESUPPORT: {
        /* "For D3D12, drivers only report the maximum level they
         * support" (d3d12umddi.h).  Same gate as 3DPIPELINESUPPORT1. */
        *(D3D12DDI_3DPIPELINELEVEL *)pArgs->pData =
            (D3D12DDI_3DPIPELINELEVEL)pipelineLevel;
        break;
    }
    case D3D12DDICAPS_TYPE_GPUVA_CAPS: {
        /* The VA width is the KMD's (DXGK_GPUMMUCAPS.VirtualAddressBitCount),
         * not the host's: the host never sees guest VAs.  Zero here kills
         * device create. */
        D3D12DDI_GPUVA_CAPS_0004 *pCaps =
            (D3D12DDI_GPUVA_CAPS_0004 *)pArgs->pData;
        CAP12_FIXED("GPUVA.MaxGPUVirtualAddressBitsPerResource",
                    pCaps->MaxGPUVirtualAddressBitsPerResource, GPUVA,
                    hc->gpuva.MaxGPUVirtualAddressBitsPerResource, 48,
                    "KMD GpuMmu VA width; host VAs are not guest VAs");
        break;
    }
    case D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE: {
        /* Virtual GPU over host system memory: the KMD's segment model
         * is UMA whatever the host backend calls itself.  Not
         * CacheCoherent: GpuMmu is bookkeeping-only and blob CPU access
         * rides the KMD-owned BAR mapping. */
        D3D12DDI_MEMORY_ARCHITECTURE_CAPS *pCaps =
            (D3D12DDI_MEMORY_ARCHITECTURE_CAPS *)pArgs->pData;
        CAP12_FIXED("MEMORY_ARCHITECTURE.UMA", pCaps->UMA, ARCH1,
                    hc->arch1.UMA, TRUE,
                    "KMD segment model: guest blobs live in host system memory");
        CAP12_FIXED("MEMORY_ARCHITECTURE.IOCoherent", pCaps->IOCoherent,
                    ARCH1, hc->arch1.UMA, TRUE, "same as UMA");
        CAP12_FIXED("MEMORY_ARCHITECTURE.CacheCoherent", pCaps->CacheCoherent,
                    ARCH1, hc->arch1.CacheCoherentUMA, FALSE,
                    "BAR-mapped blob access has no GPU cache coherency contract");
        break;
    }
    case D3D12DDICAPS_TYPE_ARCHITECTURE_INFO: {
        /* Deliberately FALSE even when the host is tile-based (the capset
         * carries TRITON_HOSTCAP_TBDR for DXMT): TRUE switches D2D, DWM
         * and XAML onto tile-deferred strategies this stack does not
         * execute correctly end to end, which costs text labels and
         * title bars in Explorer and the Start menu.  Same answer as the
         * D3D11 UMD's ARCHITECTURE_INFO. */
        D3D12DDI_ARCHITECTURE_INFO_DATA *pCaps =
            (D3D12DDI_ARCHITECTURE_INFO_DATA *)pArgs->pData;
        const BOOL hostTbdr = (hostCaps & TRITON_HOSTCAP_TBDR) ||
                              (HC12(ARCH1) && hc->arch1.TileBasedRenderer);
        TRITON_CAP_TODO("d3d12", "ARCHITECTURE_INFO.TileBasedDeferredRenderer",
                        pCaps->TileBasedDeferredRenderer, TRUE, hostTbdr, FALSE,
                        "validate tile-deferred D2D/DWM/XAML paths before reporting TRUE");
        break;
    }
    case D3D12DDICAPS_TYPE_D3D12_OPTIONS: {
        D3D12DDI_D3D12_OPTIONS_DATA_0003 *pCaps =
            (D3D12DDI_D3D12_OPTIONS_DATA_0003 *)pArgs->pData;
        CAP12_HOST("OPTIONS.ResourceBindingTier", pCaps->ResourceBindingTier,
                   OPTIONS, hc->options.ResourceBindingTier);
        CAP12_HOST("OPTIONS.ResourceHeapTier", pCaps->ResourceHeapTier,
                   OPTIONS, hc->options.ResourceHeapTier);
        CAP12_HOST("OPTIONS.OutputMergerLogicOp", pCaps->OutputMergerLogicOp,
                   OPTIONS, hc->options.OutputMergerLogicOp);
        CAP12_HOST("OPTIONS.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer",
                   pCaps->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation,
                   OPTIONS,
                   hc->options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation);
        CAP12_FIXED("OPTIONS.CrossNodeSharingTier", pCaps->CrossNodeSharingTier,
                    OPTIONS, hc->options.CrossNodeSharingTier,
                    D3D12DDI_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED,
                    "single node");
        if (hostCaps & TRITON_HOSTCAP_DXIL) {
            /* Tiled tier 2 is an FL 12_0 requirement.  It is backed either
             * by the host's sparse tier (forwarded reserved resources,
             * tritonResource12.c) or by the committed-backing shim
             * (reserved resources fully backed at create, UpdateTileMappings
             * a no-op, unmapped-reads-zero trivially true), so the claim
             * holds whatever the host reports. */
            CAP12_FIXED("OPTIONS.TiledResourcesTier", pCaps->TiledResourcesTier,
                        OPTIONS, hc->options.TiledResourcesTier,
                        D3D12DDI_TILED_RESOURCES_TIER_2,
                        "FL 12_0 requirement; committed-backing shim covers a host below tier 2");
            /* Tier 1 is claimed for the FL 12_1 label alone, which apps
             * check before they will start.  Metal has no conservative-
             * raster primitive, so a PSO that actually enables it fails
             * at create -- loud and per-PSO, never silent corruption. */
            CAP12_FIXED("OPTIONS.ConservativeRasterizationTier",
                        pCaps->ConservativeRasterizationTier, OPTIONS,
                        hc->options.ConservativeRasterizationTier,
                        D3D12DDI_CONSERVATIVE_RASTERIZATION_TIER_1,
                        "FL 12_1 label; a PSO enabling it fails at create");
        } else {
            /* D3D12DDI_TILED_RESOURCES_TIER stops at 3; the API tier 4
             * has no encoding. */
            const UINT tiledHost = hc->options.TiledResourcesTier;
            CAP12_FIXED("OPTIONS.TiledResourcesTier", pCaps->TiledResourcesTier,
                        OPTIONS, tiledHost,
                        tiledHost > (UINT)D3D12DDI_TILED_RESOURCES_TIER_3
                            ? (UINT)D3D12DDI_TILED_RESOURCES_TIER_3
                            : tiledHost,
                        "host tier, clamped to the DDI ceiling of 3");
            CAP12_HOST("OPTIONS.ConservativeRasterizationTier",
                       pCaps->ConservativeRasterizationTier, OPTIONS,
                       hc->options.ConservativeRasterizationTier);
        }
        /* Later revisions grow this struct in place (d3d12umddi.h ladder
         * _0025 .. _0081); each step is guarded by DataSize so the fills
         * light up as the advertised DDI revision moves, and the host
         * values they would carry are in the ledger either way. */
#define OPT12_AT(ver) (pArgs->DataSize >= sizeof(D3D12DDI_D3D12_OPTIONS_DATA_##ver))
        if (OPT12_AT(0025)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0025 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0025 *)pArgs->pData;
            /* Host executes it from the PSO; no guest-side work. */
            CAP12_HOST("OPTIONS.DepthBoundsTestSupported",
                       o->DepthBoundsTestSupported, OPTIONS2,
                       hc->options2.DepthBoundsTestSupported);
        }
        if (OPT12_AT(0027)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0027 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0027 *)pArgs->pData;
            CAP12_TODO("OPTIONS.ProgrammableSamplePositionsTier",
                       o->ProgrammableSamplePositionsTier, OPTIONS2,
                       hc->options2.ProgrammableSamplePositionsTier, 0,
                       "pfnSetSamplePositions (command list _0030+) not implemented");
        }
        if (OPT12_AT(0031)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0031 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0031 *)pArgs->pData;
            CAP12_HOST("OPTIONS.CopyQueueTimestampQueriesSupported",
                       o->CopyQueueTimestampQueriesSupported, OPTIONS3,
                       hc->options3.CopyQueueTimestampQueriesSupported);
        }
        if (OPT12_AT(0032)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0032 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0032 *)pArgs->pData;
            CAP12_TODO("OPTIONS.WriteBufferImmediateQueueFlags",
                       o->WriteBufferImmediateQueueFlags, OPTIONS3,
                       hc->options3.WriteBufferImmediateSupportFlags, 0,
                       "pfnWriteBufferImmediate (command list _0032+) not implemented");
        }
        if (OPT12_AT(0033)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0033 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0033 *)pArgs->pData;
            CAP12_TODO("OPTIONS.ViewInstancingTier", o->ViewInstancingTier,
                       OPTIONS3, hc->options3.ViewInstancingTier, 0,
                       "pfnSetViewInstanceMask + PSO view-instancing desc not implemented");
            /* Shader-side: SV_Barycentrics is evaluated by the host. */
            CAP12_HOST("OPTIONS.BarycentricsSupported", o->BarycentricsSupported,
                       OPTIONS3, hc->options3.BarycentricsSupported);
        }
        if (OPT12_AT(0041)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0041 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0041 *)pArgs->pData;
            /* Despite the field name, the runtime consumes this only as
             * the API MSAA64KBAlignedTextureSupported cap (d3d12umddi.h:
             * "Actually just 64KB aligned MSAA support").  The host
             * executes all placement, and the heap path already passes
             * the API-legal 64KB alignment through. */
            CAP12_HOST("OPTIONS.ReservedBufferPlacementSupported",
                       o->ReservedBufferPlacementSupported, OPTIONS4,
                       hc->options4.MSAA64KBAlignedTextureSupported);
            CAP12_FIXED("OPTIONS.Deterministic64KBUndefinedSwizzle",
                        o->Deterministic64KBUndefinedSwizzle, OPTIONS,
                        hc->options.StandardSwizzle64KBSupported, FALSE,
                        "row-major only (TEXTURE_LAYOUT)");
        }
        if (OPT12_AT(0052)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0052 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0052 *)pArgs->pData;
            CAP12_HOST("OPTIONS.SRVOnlyTiledResourceTier3",
                       o->SRVOnlyTiledResourceTier3, OPTIONS5,
                       hc->options5.SRVOnlyTiledResourceTier3);
        }
        if (OPT12_AT(0053)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0053 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0053 *)pArgs->pData;
            CAP12_TODO("OPTIONS.RenderPassTier", o->RenderPassTier, OPTIONS5,
                       hc->options5.RenderPassesTier, 0,
                       "pfnBeginRenderPass/pfnEndRenderPass not implemented");
        }
        if (OPT12_AT(0054)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0054 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0054 *)pArgs->pData;
            CAP12_TODO("OPTIONS.RaytracingTier", o->RaytracingTier, OPTIONS5,
                       hc->options5.RaytracingTier, 0,
                       "state objects, acceleration structures and DispatchRays DDI not implemented");
        }
        if (OPT12_AT(0062)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0062 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0062 *)pArgs->pData;
            CAP12_TODO("OPTIONS.VariableShadingRateTier",
                       o->VariableShadingRateTier, OPTIONS6,
                       hc->options6.VariableShadingRateTier, 0,
                       "pfnRSSetShadingRate(Image) not implemented");
            CAP12_HOST("OPTIONS.PerPrimitiveShadingRateSupportedWithViewportIndexing",
                       o->PerPrimitiveShadingRateSupportedWithViewportIndexing,
                       OPTIONS6,
                       hc->options6.PerPrimitiveShadingRateSupportedWithViewportIndexing);
            CAP12_HOST("OPTIONS.AdditionalShadingRatesSupported",
                       o->AdditionalShadingRatesSupported, OPTIONS6,
                       hc->options6.AdditionalShadingRatesSupported);
            CAP12_HOST("OPTIONS.ShadingRateImageTileSize",
                       o->ShadingRateImageTileSize, OPTIONS6,
                       hc->options6.ShadingRateImageTileSize);
            CAP12_TODO("OPTIONS.BackgroundProcessingSupported",
                       o->BackgroundProcessingSupported, OPTIONS6,
                       hc->options6.BackgroundProcessingSupported, FALSE,
                       "pfnSetBackgroundProcessingMode + pfnQueueBackgroundProcessingWorkCb not implemented");
        }
        if (OPT12_AT(0073)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0073 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0073 *)pArgs->pData;
            CAP12_TODO("OPTIONS.MeshShaderTier", o->MeshShaderTier, OPTIONS7,
                       hc->options7.MeshShaderTier, 0,
                       "pfnCreateMeshShader/pfnCreateAmplificationShader/pfnDispatchMesh not implemented");
            CAP12_TODO("OPTIONS.SamplerFeedbackTier", o->SamplerFeedbackTier,
                       OPTIONS7, hc->options7.SamplerFeedbackTier, 0,
                       "pfnCreateSamplerFeedbackUnorderedAccessView not implemented");
        }
        if (OPT12_AT(0080)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0080 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0080 *)pArgs->pData;
            CAP12_FIXED("OPTIONS.DriverManagedShaderCachePresent",
                        o->DriverManagedShaderCachePresent, OPTIONS, 0, TRUE,
                        "shaders compile host-side; both backends keep their own caches");
        }
        if (OPT12_AT(0081)) {
            D3D12DDI_D3D12_OPTIONS_DATA_0081 *o =
                (D3D12DDI_D3D12_OPTIONS_DATA_0081 *)pArgs->pData;
            CAP12_TODO("OPTIONS.MeshShaderSupportsFullRangeRenderTargetArrayIndex",
                       o->MeshShaderSupportsFullRangeRenderTargetArrayIndex,
                       OPTIONS9,
                       hc->options9.MeshShaderSupportsFullRangeRenderTargetArrayIndex,
                       FALSE, "mesh shader DDI not implemented");
        }
#undef OPT12_AT
        break;
    }
    case D3D12DDICAPS_TYPE_SHADER: {
        /* All size variants share the _0012 prefix; the runtime sizes
         * the buffer to the negotiated DDI revision (40 bytes = _0015 at
         * _0022), so the later fields are guarded by DataSize and the
         * host caps they would carry are logged as unreachable instead.
         * The wave fields must be honest either way: zero lane counts
         * read as an invalid topology and the runtime rejects the
         * adapter. */
        if (pArgs->DataSize >= sizeof(D3D12DDI_SHADER_CAPS_0012)) {
            D3D12DDI_SHADER_CAPS_0012 *pCaps =
                (D3D12DDI_SHADER_CAPS_0012 *)pArgs->pData;
            /* Both signature readers take the ISG1 chunk min-precision
             * DXBC carries; the DDI and API flag encodings agree (10_BIT
             * = 1, 16_BIT = 2).  Backed on both sides by the same fxc
             * min16float draw: sm6draw.exe -minprec (guest) and
             * d3d12-minprec-test (host). */
            CAP12_HOST("SHADER.MinPrecision", pCaps->MinPrecision, OPTIONS,
                       hc->options.MinPrecisionSupport);
            CAP12_HOST("SHADER.Doubles", pCaps->Doubles, OPTIONS,
                       hc->options.DoublePrecisionFloatShaderOps);
            CAP12_HOST("SHADER.ShaderSpecifiedStencilRef",
                       pCaps->ShaderSpecifiedStencilRef, OPTIONS,
                       hc->options.PSSpecifiedStencilRefSupported);
            /* Backed per format by UAV_READS/UAV_WRITES in
             * triton12CheckFormatSupport -- keep the two in sync. */
            CAP12_HOST("SHADER.TypedUAVLoadAdditionalFormats",
                       pCaps->TypedUAVLoadAdditionalFormats, OPTIONS,
                       hc->options.TypedUAVLoadAdditionalFormats);
            CAP12_HOST("SHADER.ROVs", pCaps->ROVs, OPTIONS,
                       hc->options.ROVsSupported);
            CAP12_HOST("SHADER.WaveOps", pCaps->WaveOps, OPTIONS1,
                       hc->options1.WaveOps);
            CAP12_HOST("SHADER.WaveLaneCountMin", pCaps->WaveLaneCountMin,
                       OPTIONS1, hc->options1.WaveLaneCountMin);
            CAP12_HOST("SHADER.WaveLaneCountMax", pCaps->WaveLaneCountMax,
                       OPTIONS1, hc->options1.WaveLaneCountMax);
            CAP12_HOST("SHADER.TotalLaneCount", pCaps->TotalLaneCount,
                       OPTIONS1, hc->options1.TotalLaneCount);
        }
        if (pArgs->DataSize >= sizeof(D3D12DDI_SHADER_CAPS_0015)) {
            D3D12DDI_SHADER_CAPS_0015 *pCaps =
                (D3D12DDI_SHADER_CAPS_0015 *)pArgs->pData;
            CAP12_HOST("SHADER.Int64Ops", pCaps->Int64Ops, OPTIONS1,
                       hc->options1.Int64ShaderOps);
        }
        if (pArgs->DataSize >= sizeof(D3D12DDI_SHADER_CAPS_0082)) {
            /* Reached only once the advertised DDI revision is >= R8;
             * shader-side features the host executes from forwarded
             * DXIL, so host truth applies as-is. */
            D3D12DDI_SHADER_CAPS_0082 *pCaps =
                (D3D12DDI_SHADER_CAPS_0082 *)pArgs->pData;
            CAP12_HOST("SHADER.Native16BitOps", pCaps->Native16BitOps,
                       OPTIONS4, hc->options4.Native16BitShaderOpsSupported);
            CAP12_HOST("SHADER.AtomicInt64OnTypedResource",
                       pCaps->AtomicInt64OnTypedResource, OPTIONS9,
                       hc->options9.AtomicInt64OnTypedResourceSupported);
            CAP12_HOST("SHADER.AtomicInt64OnGroupShared",
                       pCaps->AtomicInt64OnGroupShared, OPTIONS9,
                       hc->options9.AtomicInt64OnGroupSharedSupported);
            CAP12_TODO("SHADER.DerivativesInMeshAndAmplificationShaders",
                       pCaps->DerivativesInMeshAndAmplificationShaders,
                       OPTIONS9,
                       hc->options9.DerivativesInMeshAndAmplificationShadersSupported,
                       FALSE, "mesh shader DDI not implemented");
            CAP12_HOST("SHADER.WaveMMATier", pCaps->WaveMMATier, OPTIONS9,
                       hc->options9.WaveMMATier);
        } else {
            const char *why = "SHADER_CAPS_0082 is DDI R8; runtime sized "
                              "the query for _0022 (see GetSupportedVersions)";
            TRITON_CAP_UNREACHABLE("d3d12", "SHADER.Native16BitOps",
                                   HC12(OPTIONS4),
                                   hc->options4.Native16BitShaderOpsSupported, why);
            TRITON_CAP_UNREACHABLE("d3d12", "SHADER.AtomicInt64OnTypedResource",
                                   HC12(OPTIONS9),
                                   hc->options9.AtomicInt64OnTypedResourceSupported, why);
            TRITON_CAP_UNREACHABLE("d3d12", "SHADER.AtomicInt64OnGroupShared",
                                   HC12(OPTIONS9),
                                   hc->options9.AtomicInt64OnGroupSharedSupported, why);
        }
        if (pArgs->DataSize >= sizeof(D3D12DDI_SHADER_CAPS_0084)) {
            D3D12DDI_SHADER_CAPS_0084 *pCaps =
                (D3D12DDI_SHADER_CAPS_0084 *)pArgs->pData;
            CAP12_HOST("SHADER.AtomicInt64OnDescriptorHeapResource",
                       pCaps->AtomicInt64OnDescriptorHeapResource, OPTIONS11,
                       hc->options11.AtomicInt64OnDescriptorHeapResourceSupported);
        } else {
            TRITON_CAP_UNREACHABLE("d3d12", "SHADER.AtomicInt64OnDescriptorHeapResource",
                                   HC12(OPTIONS11),
                                   hc->options11.AtomicInt64OnDescriptorHeapResourceSupported,
                                   "SHADER_CAPS_0084 is DDI R8; runtime sized the query for _0022");
        }
        TRITON_CAP_UNREACHABLE("d3d12", "OPTIONS9.MeshShaderPipelineStatsSupported",
                               HC12(OPTIONS9),
                               hc->options9.MeshShaderPipelineStatsSupported,
                               "no D3D12DDI field at any revision in WDK 26100");
        break;
    }
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT:
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT1: {
        /* Row-major only: no device-dependent layouts, no standard
         * swizzle.  The _0001 variant carries SupportsRowMajorTexture;
         * discriminate by DataSize.  CacheHeapCaps asks through the
         * TEXTURE_LAYOUT1 alias and a zero fill there fails device
         * creation, so both types share this answer. */
        if (pArgs->DataSize >= sizeof(D3D12DDI_TEXTURE_LAYOUT_CAPS_0001)) {
            D3D12DDI_TEXTURE_LAYOUT_CAPS_0001 *pCaps =
                (D3D12DDI_TEXTURE_LAYOUT_CAPS_0001 *)pArgs->pData;
            pCaps->SupportsRowMajorTexture = TRUE;
        }
        break;
    }
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS: {
        /* pInfo = UINT[2]{ D3D12DDI_TL_ROW_MAJOR, FUNCTIONAL_UNIT },
         * pData = D3D12DDI_ROW_MAJOR_LAYOUT_CAPS.  CacheHeapCaps fails
         * device creation on all-zero sub-caps, so answer the standard
         * D3D12 row-major rules: 256B row pitch alignment, 512B
         * placement alignment, texels up to 16B. */
        if (pArgs->DataSize >= sizeof(D3D12DDI_ROW_MAJOR_LAYOUT_CAPS)) {
            D3D12DDI_ROW_MAJOR_LAYOUT_CAPS *pCaps =
                (D3D12DDI_ROW_MAJOR_LAYOUT_CAPS *)pArgs->pData;
            for (int i = 0; i < 2; i++) {
                pCaps->SubCaps[i].MaxElementSize      = 16;
                pCaps->SubCaps[i].BaseOffsetAlignment = 512;
                pCaps->SubCaps[i].PitchAlignment      = 256;
                /* CacheHeapCaps validates this against a 0x100 ceiling
                 * and requires a non-zero power of two, so 512 fails. */
                pCaps->SubCaps[i].DepthPitchAlignment = 256;
            }
            pCaps->Flags = D3D12DDI_ROW_MAJOR_LAYOUT_FLAG_NONE;
        }
        break;
    }
    case D3D12DDICAPS_TYPE_0022_CPU_PAGE_TABLE_FALSE_POSITIVES: {
        /* pData = D3D12DDI_COMMAND_QUEUE_FLAGS of the queue types whose
         * CPU page-table walks can report false positives.  Guest VAs are
         * resolved by the KMD's GpuMmu, never by a host page walk. */
        if (pArgs->DataSize >= sizeof(UINT))
            *(UINT *)pArgs->pData = D3D12DDI_COMMAND_QUEUE_FLAG_NONE;
        break;
    }
    case D3D12DDICAPS_TYPE_EXECUTECOMMANDLISTS_PARALLELISM: {
        /* pData = BOOL.  Every queue's ExecuteCommandLists serialises on
         * the device QueueLock and one ring (npt_entry_d3d12.c), so the
         * runtime gains nothing from calling it in parallel. */
        if (pArgs->DataSize >= sizeof(BOOL)) {
            BOOL *pCaps = (BOOL *)pArgs->pData;
            TRITON_CAP_TODO("d3d12", "EXECUTECOMMANDLISTS_PARALLELISM", *pCaps,
                            FALSE, 0, FALSE,
                            "ExecuteCommandLists is serialised on QueueLock + one ring");
        }
        break;
    }
    case D3D12DDICAPS_TYPE_0073_SUPPORT_BATCHED_MARKERS: {
        /* pData = BOOL; no marker DDI is implemented. */
        if (pArgs->DataSize >= sizeof(BOOL))
            *(BOOL *)pArgs->pData = FALSE;
        break;
    }
    default:
        /* Zero-filled, i.e. feature off. */
        break;
    }
    return S_OK;
}

#undef HC12
#undef CAP12_HOST
#undef CAP12_FIXED
#undef CAP12_TODO


/* ---------- table stubs ----------
 *
 * Every DDI table slot without a real body gets a stub that names its
 * index once, so an unimplemented entry the runtime reaches is visible
 * instead of a jump through uninitialised memory.  The uniform
 * four-argument shape is a calling-convention pun: on x64 the first
 * four arguments arrive in registers and the integer return leaves in
 * RAX, so returning 0 reads as S_OK, zero size or NULL according to the
 * slot's real signature.
 *
 * A slot returning a struct BY VALUE cannot be stubbed this way -- the
 * hidden return-buffer pointer shifts the arguments -- so those need
 * real bodies. */
static UINT64 APIENTRY t12_stub_0(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_0; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_0, 1)) TR_LOG("12.STUB[%d] hit", 0); return 0; }
static UINT64 APIENTRY t12_stub_1(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_1; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_1, 1)) TR_LOG("12.STUB[%d] hit", 1); return 0; }
static UINT64 APIENTRY t12_stub_2(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_2; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_2, 1)) TR_LOG("12.STUB[%d] hit", 2); return 0; }
static UINT64 APIENTRY t12_stub_3(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_3; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_3, 1)) TR_LOG("12.STUB[%d] hit", 3); return 0; }
static UINT64 APIENTRY t12_stub_4(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_4; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_4, 1)) TR_LOG("12.STUB[%d] hit", 4); return 0; }
static UINT64 APIENTRY t12_stub_5(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_5; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_5, 1)) TR_LOG("12.STUB[%d] hit", 5); return 0; }
static UINT64 APIENTRY t12_stub_6(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_6; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_6, 1)) TR_LOG("12.STUB[%d] hit", 6); return 0; }
static UINT64 APIENTRY t12_stub_7(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_7; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_7, 1)) TR_LOG("12.STUB[%d] hit", 7); return 0; }
static UINT64 APIENTRY t12_stub_8(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_8; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_8, 1)) TR_LOG("12.STUB[%d] hit", 8); return 0; }
static UINT64 APIENTRY t12_stub_9(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_9; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_9, 1)) TR_LOG("12.STUB[%d] hit", 9); return 0; }
static UINT64 APIENTRY t12_stub_10(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_10; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_10, 1)) TR_LOG("12.STUB[%d] hit", 10); return 0; }
static UINT64 APIENTRY t12_stub_11(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_11; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_11, 1)) TR_LOG("12.STUB[%d] hit", 11); return 0; }
static UINT64 APIENTRY t12_stub_12(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_12; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_12, 1)) TR_LOG("12.STUB[%d] hit", 12); return 0; }
static UINT64 APIENTRY t12_stub_13(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_13; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_13, 1)) TR_LOG("12.STUB[%d] hit", 13); return 0; }
static UINT64 APIENTRY t12_stub_14(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_14; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_14, 1)) TR_LOG("12.STUB[%d] hit", 14); return 0; }
static UINT64 APIENTRY t12_stub_15(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_15; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_15, 1)) TR_LOG("12.STUB[%d] hit", 15); return 0; }
static UINT64 APIENTRY t12_stub_16(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_16; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_16, 1)) TR_LOG("12.STUB[%d] hit", 16); return 0; }
static UINT64 APIENTRY t12_stub_17(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_17; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_17, 1)) TR_LOG("12.STUB[%d] hit", 17); return 0; }
static UINT64 APIENTRY t12_stub_18(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_18; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_18, 1)) TR_LOG("12.STUB[%d] hit", 18); return 0; }
static UINT64 APIENTRY t12_stub_19(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_19; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_19, 1)) TR_LOG("12.STUB[%d] hit", 19); return 0; }
static UINT64 APIENTRY t12_stub_20(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_20; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_20, 1)) TR_LOG("12.STUB[%d] hit", 20); return 0; }
static UINT64 APIENTRY t12_stub_21(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_21; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_21, 1)) TR_LOG("12.STUB[%d] hit", 21); return 0; }
static UINT64 APIENTRY t12_stub_22(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_22; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_22, 1)) TR_LOG("12.STUB[%d] hit", 22); return 0; }
static UINT64 APIENTRY t12_stub_23(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_23; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_23, 1)) TR_LOG("12.STUB[%d] hit", 23); return 0; }
static UINT64 APIENTRY t12_stub_24(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_24; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_24, 1)) TR_LOG("12.STUB[%d] hit", 24); return 0; }
static UINT64 APIENTRY t12_stub_25(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_25; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_25, 1)) TR_LOG("12.STUB[%d] hit", 25); return 0; }
static UINT64 APIENTRY t12_stub_26(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_26; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_26, 1)) TR_LOG("12.STUB[%d] hit", 26); return 0; }
static UINT64 APIENTRY t12_stub_27(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_27; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_27, 1)) TR_LOG("12.STUB[%d] hit", 27); return 0; }
static UINT64 APIENTRY t12_stub_28(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_28; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_28, 1)) TR_LOG("12.STUB[%d] hit", 28); return 0; }
static UINT64 APIENTRY t12_stub_29(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_29; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_29, 1)) TR_LOG("12.STUB[%d] hit", 29); return 0; }
static UINT64 APIENTRY t12_stub_30(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_30; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_30, 1)) TR_LOG("12.STUB[%d] hit", 30); return 0; }
static UINT64 APIENTRY t12_stub_31(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_31; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_31, 1)) TR_LOG("12.STUB[%d] hit", 31); return 0; }
static UINT64 APIENTRY t12_stub_32(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_32; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_32, 1)) TR_LOG("12.STUB[%d] hit", 32); return 0; }
static UINT64 APIENTRY t12_stub_33(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_33; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_33, 1)) TR_LOG("12.STUB[%d] hit", 33); return 0; }
static UINT64 APIENTRY t12_stub_34(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_34; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_34, 1)) TR_LOG("12.STUB[%d] hit", 34); return 0; }
static UINT64 APIENTRY t12_stub_35(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_35; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_35, 1)) TR_LOG("12.STUB[%d] hit", 35); return 0; }
static UINT64 APIENTRY t12_stub_36(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_36; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_36, 1)) TR_LOG("12.STUB[%d] hit", 36); return 0; }
static UINT64 APIENTRY t12_stub_37(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_37; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_37, 1)) TR_LOG("12.STUB[%d] hit", 37); return 0; }
static UINT64 APIENTRY t12_stub_38(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_38; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_38, 1)) TR_LOG("12.STUB[%d] hit", 38); return 0; }
static UINT64 APIENTRY t12_stub_39(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_39; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_39, 1)) TR_LOG("12.STUB[%d] hit", 39); return 0; }
static UINT64 APIENTRY t12_stub_40(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_40; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_40, 1)) TR_LOG("12.STUB[%d] hit", 40); return 0; }
static UINT64 APIENTRY t12_stub_41(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_41; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_41, 1)) TR_LOG("12.STUB[%d] hit", 41); return 0; }
static UINT64 APIENTRY t12_stub_42(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_42; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_42, 1)) TR_LOG("12.STUB[%d] hit", 42); return 0; }
static UINT64 APIENTRY t12_stub_43(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_43; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_43, 1)) TR_LOG("12.STUB[%d] hit", 43); return 0; }
static UINT64 APIENTRY t12_stub_44(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_44; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_44, 1)) TR_LOG("12.STUB[%d] hit", 44); return 0; }
static UINT64 APIENTRY t12_stub_45(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_45; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_45, 1)) TR_LOG("12.STUB[%d] hit", 45); return 0; }
static UINT64 APIENTRY t12_stub_46(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_46; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_46, 1)) TR_LOG("12.STUB[%d] hit", 46); return 0; }
static UINT64 APIENTRY t12_stub_47(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_47; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_47, 1)) TR_LOG("12.STUB[%d] hit", 47); return 0; }
static UINT64 APIENTRY t12_stub_48(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_48; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_48, 1)) TR_LOG("12.STUB[%d] hit", 48); return 0; }
static UINT64 APIENTRY t12_stub_49(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_49; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_49, 1)) TR_LOG("12.STUB[%d] hit", 49); return 0; }
static UINT64 APIENTRY t12_stub_50(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_50; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_50, 1)) TR_LOG("12.STUB[%d] hit", 50); return 0; }
static UINT64 APIENTRY t12_stub_51(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_51; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_51, 1)) TR_LOG("12.STUB[%d] hit", 51); return 0; }
static UINT64 APIENTRY t12_stub_52(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_52; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_52, 1)) TR_LOG("12.STUB[%d] hit", 52); return 0; }
static UINT64 APIENTRY t12_stub_53(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_53; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_53, 1)) TR_LOG("12.STUB[%d] hit", 53); return 0; }
static UINT64 APIENTRY t12_stub_54(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_54; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_54, 1)) TR_LOG("12.STUB[%d] hit", 54); return 0; }
static UINT64 APIENTRY t12_stub_55(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_55; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_55, 1)) TR_LOG("12.STUB[%d] hit", 55); return 0; }
static UINT64 APIENTRY t12_stub_56(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_56; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_56, 1)) TR_LOG("12.STUB[%d] hit", 56); return 0; }
static UINT64 APIENTRY t12_stub_57(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_57; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_57, 1)) TR_LOG("12.STUB[%d] hit", 57); return 0; }
static UINT64 APIENTRY t12_stub_58(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_58; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_58, 1)) TR_LOG("12.STUB[%d] hit", 58); return 0; }
static UINT64 APIENTRY t12_stub_59(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_59; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_59, 1)) TR_LOG("12.STUB[%d] hit", 59); return 0; }
static UINT64 APIENTRY t12_stub_60(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_60; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_60, 1)) TR_LOG("12.STUB[%d] hit", 60); return 0; }
static UINT64 APIENTRY t12_stub_61(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_61; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_61, 1)) TR_LOG("12.STUB[%d] hit", 61); return 0; }
static UINT64 APIENTRY t12_stub_62(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_62; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_62, 1)) TR_LOG("12.STUB[%d] hit", 62); return 0; }
static UINT64 APIENTRY t12_stub_63(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_63; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_63, 1)) TR_LOG("12.STUB[%d] hit", 63); return 0; }
static UINT64 APIENTRY t12_stub_64(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_64; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_64, 1)) TR_LOG("12.STUB[%d] hit", 64); return 0; }
static UINT64 APIENTRY t12_stub_65(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_65; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_65, 1)) TR_LOG("12.STUB[%d] hit", 65); return 0; }
static UINT64 APIENTRY t12_stub_66(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_66; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_66, 1)) TR_LOG("12.STUB[%d] hit", 66); return 0; }
static UINT64 APIENTRY t12_stub_67(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_67; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_67, 1)) TR_LOG("12.STUB[%d] hit", 67); return 0; }
static UINT64 APIENTRY t12_stub_68(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_68; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_68, 1)) TR_LOG("12.STUB[%d] hit", 68); return 0; }
static UINT64 APIENTRY t12_stub_69(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_69; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_69, 1)) TR_LOG("12.STUB[%d] hit", 69); return 0; }
static UINT64 APIENTRY t12_stub_70(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_70; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_70, 1)) TR_LOG("12.STUB[%d] hit", 70); return 0; }
static UINT64 APIENTRY t12_stub_71(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_71; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_71, 1)) TR_LOG("12.STUB[%d] hit", 71); return 0; }
static UINT64 APIENTRY t12_stub_72(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_72; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_72, 1)) TR_LOG("12.STUB[%d] hit", 72); return 0; }
static UINT64 APIENTRY t12_stub_73(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_73; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_73, 1)) TR_LOG("12.STUB[%d] hit", 73); return 0; }
static UINT64 APIENTRY t12_stub_74(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_74; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_74, 1)) TR_LOG("12.STUB[%d] hit", 74); return 0; }
static UINT64 APIENTRY t12_stub_75(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_75; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_75, 1)) TR_LOG("12.STUB[%d] hit", 75); return 0; }
static UINT64 APIENTRY t12_stub_76(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_76; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_76, 1)) TR_LOG("12.STUB[%d] hit", 76); return 0; }
static UINT64 APIENTRY t12_stub_77(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_77; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_77, 1)) TR_LOG("12.STUB[%d] hit", 77); return 0; }
static UINT64 APIENTRY t12_stub_78(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_78; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_78, 1)) TR_LOG("12.STUB[%d] hit", 78); return 0; }
static UINT64 APIENTRY t12_stub_79(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_79; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_79, 1)) TR_LOG("12.STUB[%d] hit", 79); return 0; }
static UINT64 APIENTRY t12_stub_80(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_80; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_80, 1)) TR_LOG("12.STUB[%d] hit", 80); return 0; }
static UINT64 APIENTRY t12_stub_81(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_81; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_81, 1)) TR_LOG("12.STUB[%d] hit", 81); return 0; }
static UINT64 APIENTRY t12_stub_82(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_82; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_82, 1)) TR_LOG("12.STUB[%d] hit", 82); return 0; }
static UINT64 APIENTRY t12_stub_83(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_83; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_83, 1)) TR_LOG("12.STUB[%d] hit", 83); return 0; }
static UINT64 APIENTRY t12_stub_84(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_84; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_84, 1)) TR_LOG("12.STUB[%d] hit", 84); return 0; }
static UINT64 APIENTRY t12_stub_85(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_85; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_85, 1)) TR_LOG("12.STUB[%d] hit", 85); return 0; }
static UINT64 APIENTRY t12_stub_86(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_86; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_86, 1)) TR_LOG("12.STUB[%d] hit", 86); return 0; }
static UINT64 APIENTRY t12_stub_87(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_87; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_87, 1)) TR_LOG("12.STUB[%d] hit", 87); return 0; }
static UINT64 APIENTRY t12_stub_88(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_88; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_88, 1)) TR_LOG("12.STUB[%d] hit", 88); return 0; }
static UINT64 APIENTRY t12_stub_89(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_89; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_89, 1)) TR_LOG("12.STUB[%d] hit", 89); return 0; }
static UINT64 APIENTRY t12_stub_90(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_90; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_90, 1)) TR_LOG("12.STUB[%d] hit", 90); return 0; }
static UINT64 APIENTRY t12_stub_91(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_91; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_91, 1)) TR_LOG("12.STUB[%d] hit", 91); return 0; }
static UINT64 APIENTRY t12_stub_92(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_92; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_92, 1)) TR_LOG("12.STUB[%d] hit", 92); return 0; }
static UINT64 APIENTRY t12_stub_93(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_93; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_93, 1)) TR_LOG("12.STUB[%d] hit", 93); return 0; }
static UINT64 APIENTRY t12_stub_94(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_94; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_94, 1)) TR_LOG("12.STUB[%d] hit", 94); return 0; }
static UINT64 APIENTRY t12_stub_95(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_95; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_95, 1)) TR_LOG("12.STUB[%d] hit", 95); return 0; }
static UINT64 APIENTRY t12_stub_96(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_96; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_96, 1)) TR_LOG("12.STUB[%d] hit", 96); return 0; }
static UINT64 APIENTRY t12_stub_97(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_97; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_97, 1)) TR_LOG("12.STUB[%d] hit", 97); return 0; }
static UINT64 APIENTRY t12_stub_98(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_98; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_98, 1)) TR_LOG("12.STUB[%d] hit", 98); return 0; }
static UINT64 APIENTRY t12_stub_99(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_99; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_99, 1)) TR_LOG("12.STUB[%d] hit", 99); return 0; }
static UINT64 APIENTRY t12_stub_100(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_100; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_100, 1)) TR_LOG("12.STUB[%d] hit", 100); return 0; }
static UINT64 APIENTRY t12_stub_101(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_101; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_101, 1)) TR_LOG("12.STUB[%d] hit", 101); return 0; }
static UINT64 APIENTRY t12_stub_102(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_102; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_102, 1)) TR_LOG("12.STUB[%d] hit", 102); return 0; }
static UINT64 APIENTRY t12_stub_103(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_103; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_103, 1)) TR_LOG("12.STUB[%d] hit", 103); return 0; }
static UINT64 APIENTRY t12_stub_104(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_104; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_104, 1)) TR_LOG("12.STUB[%d] hit", 104); return 0; }
static UINT64 APIENTRY t12_stub_105(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_105; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_105, 1)) TR_LOG("12.STUB[%d] hit", 105); return 0; }
static UINT64 APIENTRY t12_stub_106(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_106; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_106, 1)) TR_LOG("12.STUB[%d] hit", 106); return 0; }
static UINT64 APIENTRY t12_stub_107(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_107; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_107, 1)) TR_LOG("12.STUB[%d] hit", 107); return 0; }
static UINT64 APIENTRY t12_stub_108(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_108; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_108, 1)) TR_LOG("12.STUB[%d] hit", 108); return 0; }
static UINT64 APIENTRY t12_stub_109(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_109; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_109, 1)) TR_LOG("12.STUB[%d] hit", 109); return 0; }
static UINT64 APIENTRY t12_stub_110(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_110; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_110, 1)) TR_LOG("12.STUB[%d] hit", 110); return 0; }
static UINT64 APIENTRY t12_stub_111(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_111; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_111, 1)) TR_LOG("12.STUB[%d] hit", 111); return 0; }
static UINT64 APIENTRY t12_stub_112(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_112; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_112, 1)) TR_LOG("12.STUB[%d] hit", 112); return 0; }
static UINT64 APIENTRY t12_stub_113(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_113; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_113, 1)) TR_LOG("12.STUB[%d] hit", 113); return 0; }
static UINT64 APIENTRY t12_stub_114(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_114; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_114, 1)) TR_LOG("12.STUB[%d] hit", 114); return 0; }
static UINT64 APIENTRY t12_stub_115(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_115; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_115, 1)) TR_LOG("12.STUB[%d] hit", 115); return 0; }
static UINT64 APIENTRY t12_stub_116(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_116; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_116, 1)) TR_LOG("12.STUB[%d] hit", 116); return 0; }
static UINT64 APIENTRY t12_stub_117(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_117; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_117, 1)) TR_LOG("12.STUB[%d] hit", 117); return 0; }
static UINT64 APIENTRY t12_stub_118(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_118; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_118, 1)) TR_LOG("12.STUB[%d] hit", 118); return 0; }
static UINT64 APIENTRY t12_stub_119(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_119; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_119, 1)) TR_LOG("12.STUB[%d] hit", 119); return 0; }
static UINT64 APIENTRY t12_stub_120(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_120; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_120, 1)) TR_LOG("12.STUB[%d] hit", 120); return 0; }
static UINT64 APIENTRY t12_stub_121(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_121; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_121, 1)) TR_LOG("12.STUB[%d] hit", 121); return 0; }
static UINT64 APIENTRY t12_stub_122(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_122; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_122, 1)) TR_LOG("12.STUB[%d] hit", 122); return 0; }
static UINT64 APIENTRY t12_stub_123(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_123; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_123, 1)) TR_LOG("12.STUB[%d] hit", 123); return 0; }
static UINT64 APIENTRY t12_stub_124(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_124; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_124, 1)) TR_LOG("12.STUB[%d] hit", 124); return 0; }
static UINT64 APIENTRY t12_stub_125(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_125; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_125, 1)) TR_LOG("12.STUB[%d] hit", 125); return 0; }
static UINT64 APIENTRY t12_stub_126(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_126; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_126, 1)) TR_LOG("12.STUB[%d] hit", 126); return 0; }
static UINT64 APIENTRY t12_stub_127(UINT64 a, UINT64 b, UINT64 c, UINT64 d)
{ static LONG t12_once_127; (void)a;(void)b;(void)c;(void)d; if (!InterlockedExchange(&t12_once_127, 1)) TR_LOG("12.STUB[%d] hit", 127); return 0; }

static void *const t12_stub_table[128] = {
    (void *)t12_stub_0,
    (void *)t12_stub_1,
    (void *)t12_stub_2,
    (void *)t12_stub_3,
    (void *)t12_stub_4,
    (void *)t12_stub_5,
    (void *)t12_stub_6,
    (void *)t12_stub_7,
    (void *)t12_stub_8,
    (void *)t12_stub_9,
    (void *)t12_stub_10,
    (void *)t12_stub_11,
    (void *)t12_stub_12,
    (void *)t12_stub_13,
    (void *)t12_stub_14,
    (void *)t12_stub_15,
    (void *)t12_stub_16,
    (void *)t12_stub_17,
    (void *)t12_stub_18,
    (void *)t12_stub_19,
    (void *)t12_stub_20,
    (void *)t12_stub_21,
    (void *)t12_stub_22,
    (void *)t12_stub_23,
    (void *)t12_stub_24,
    (void *)t12_stub_25,
    (void *)t12_stub_26,
    (void *)t12_stub_27,
    (void *)t12_stub_28,
    (void *)t12_stub_29,
    (void *)t12_stub_30,
    (void *)t12_stub_31,
    (void *)t12_stub_32,
    (void *)t12_stub_33,
    (void *)t12_stub_34,
    (void *)t12_stub_35,
    (void *)t12_stub_36,
    (void *)t12_stub_37,
    (void *)t12_stub_38,
    (void *)t12_stub_39,
    (void *)t12_stub_40,
    (void *)t12_stub_41,
    (void *)t12_stub_42,
    (void *)t12_stub_43,
    (void *)t12_stub_44,
    (void *)t12_stub_45,
    (void *)t12_stub_46,
    (void *)t12_stub_47,
    (void *)t12_stub_48,
    (void *)t12_stub_49,
    (void *)t12_stub_50,
    (void *)t12_stub_51,
    (void *)t12_stub_52,
    (void *)t12_stub_53,
    (void *)t12_stub_54,
    (void *)t12_stub_55,
    (void *)t12_stub_56,
    (void *)t12_stub_57,
    (void *)t12_stub_58,
    (void *)t12_stub_59,
    (void *)t12_stub_60,
    (void *)t12_stub_61,
    (void *)t12_stub_62,
    (void *)t12_stub_63,
    (void *)t12_stub_64,
    (void *)t12_stub_65,
    (void *)t12_stub_66,
    (void *)t12_stub_67,
    (void *)t12_stub_68,
    (void *)t12_stub_69,
    (void *)t12_stub_70,
    (void *)t12_stub_71,
    (void *)t12_stub_72,
    (void *)t12_stub_73,
    (void *)t12_stub_74,
    (void *)t12_stub_75,
    (void *)t12_stub_76,
    (void *)t12_stub_77,
    (void *)t12_stub_78,
    (void *)t12_stub_79,
    (void *)t12_stub_80,
    (void *)t12_stub_81,
    (void *)t12_stub_82,
    (void *)t12_stub_83,
    (void *)t12_stub_84,
    (void *)t12_stub_85,
    (void *)t12_stub_86,
    (void *)t12_stub_87,
    (void *)t12_stub_88,
    (void *)t12_stub_89,
    (void *)t12_stub_90,
    (void *)t12_stub_91,
    (void *)t12_stub_92,
    (void *)t12_stub_93,
    (void *)t12_stub_94,
    (void *)t12_stub_95,
    (void *)t12_stub_96,
    (void *)t12_stub_97,
    (void *)t12_stub_98,
    (void *)t12_stub_99,
    (void *)t12_stub_100,
    (void *)t12_stub_101,
    (void *)t12_stub_102,
    (void *)t12_stub_103,
    (void *)t12_stub_104,
    (void *)t12_stub_105,
    (void *)t12_stub_106,
    (void *)t12_stub_107,
    (void *)t12_stub_108,
    (void *)t12_stub_109,
    (void *)t12_stub_110,
    (void *)t12_stub_111,
    (void *)t12_stub_112,
    (void *)t12_stub_113,
    (void *)t12_stub_114,
    (void *)t12_stub_115,
    (void *)t12_stub_116,
    (void *)t12_stub_117,
    (void *)t12_stub_118,
    (void *)t12_stub_119,
    (void *)t12_stub_120,
    (void *)t12_stub_121,
    (void *)t12_stub_122,
    (void *)t12_stub_123,
    (void *)t12_stub_124,
    (void *)t12_stub_125,
    (void *)t12_stub_126,
    (void *)t12_stub_127
};


/* ---------- device core (table type 0) ----------
 *
 * Format and multisample answers forward to the inner device and then
 * obey the same three rules as the D3D11 path in tritonQuery.c: report
 * the minimal DDI bit set, keep MULTISAMPLE_RENDERTARGET exactly
 * consistent with the quality-level query, and strip colour caps from
 * typed depth formats.  The runtime cross-checks all three during
 * device finalization. */

/* Cap-query results are constant per host device but re-queried at
 * arbitrary frequency, and each query is a synchronous host round trip;
 * answer re-queries from a local cache (same rationale as the D3D11
 * caches in tritonQuery.c).
 *
 * Validity lives in bit 32 of a single 64-bit entry so a reader
 * observes value and validity through one aligned load.  Split
 * valid/value arrays would need the publish's two stores ordered
 * against the read's two loads: on ARM64 the validity store may become
 * visible first and hand back a zero mask, which the runtime then
 * latches.  The D3D11 caches keep validity in bit 31 of a 32-bit entry
 * instead, but the D3D12 value half must carry
 * D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED (0x80000000), so bit 31 is not
 * free here. */
#define T12_QCACHE_VALID (1ll << 32)
static volatile LONG64 s_fmt12Cache[256];
static volatile LONG64 s_msaa12Cache[256][33];

/* Formats whose MSAA-rendertarget class is NEVER in the runtime's own
 * static format table, indexed by DXGI_FORMAT.  The finalization
 * cast-set walk throws DXGI_ERROR_DRIVER_INTERNAL_ERROR -- usually with
 * no journal string -- whenever a driver claims the
 * MULTISAMPLE_RENDERTARGET bit or a nonzero quality ladder on one of
 * them, and the host does report quality levels for several, so its
 * answer has to be gated here.
 *
 * Mirrored from D3D12Core's per-feature-level format tables (bits 13:12
 * of each 12-byte record; NEVER in any one table counts), so it must be
 * rechecked when the target runtime version moves. */
static const UINT8 s_msaa12Never[192] = {
    1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0,
    0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,
};

static UINT
t12MsaaQuality(PTRITON12_DEVICE p, DXGI_FORMAT Format, UINT SampleCount)
{
    if ((UINT)Format < 256u && SampleCount <= 32u) {
        const LONG64 e = s_msaa12Cache[Format][SampleCount];
        if (e & T12_QCACHE_VALID)
            return (UINT)e;
    }

    /* Gate by the runtime's static NEVER-MSAA class (s_msaa12Never above),
     * EXCEPT the typeless family parents: the cast-set walk uses the
     * TYPELESS format as the root whose quality ladder every claiming
     * member must match ("MSAA quality for parent != child" throws when a
     * gated root answers 0 while R32G32B32A32_FLOAT answers 1 -- seen the
     * moment the gate covered fmt 1).  Typeless parents of MSAA-capable
     * families must therefore answer the FAMILY ladder (the host answers
     * typeless queries family-consistently; depth-typeless lifts below).
     * BC*_TYPELESS stay gated: their whole family is NEVER, so a zero
     * root and zero members stay consistent. */
    switch (Format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        break;
    default:
        if ((UINT)Format < 192u && s_msaa12Never[Format])
            return (SampleCount <= 1) ? 1 : 0;
        break;
    }

    UINT n = 0;
    BOOL fromHost = FALSE;
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q;
    q.Format = Format;
    q.SampleCount = SampleCount;
    q.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    q.NumQualityLevels = 0;
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(
            p->pDev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q)))) {
        n = q.NumQualityLevels;
        fromHost = TRUE;
    }

    /* Typeless depth parents map to plain host formats with no depth
     * MSAA, so take the typed depth member's answer instead. */
    DXGI_FORMAT typedDepth = DXGI_FORMAT_UNKNOWN;
    switch (Format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: typedDepth = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; break;
    case DXGI_FORMAT_R32_TYPELESS:      typedDepth = DXGI_FORMAT_D32_FLOAT;            break;
    case DXGI_FORMAT_R24G8_TYPELESS:    typedDepth = DXGI_FORMAT_D24_UNORM_S8_UINT;    break;
    case DXGI_FORMAT_R16_TYPELESS:      typedDepth = DXGI_FORMAT_D16_UNORM;            break;
    default: break;
    }
    if (typedDepth != DXGI_FORMAT_UNKNOWN) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qd;
        qd.Format = typedDepth;
        qd.SampleCount = SampleCount;
        qd.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        qd.NumQualityLevels = 0;
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(
                p->pDev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qd,
                sizeof(qd))) && qd.NumQualityLevels > n)
            n = qd.NumQualityLevels;
    }

    /* A failed round trip (DEVICE_REMOVED, host rejection) is not a
     * host answer; a zero cached from it would outlive the failure. */
    if (fromHost && (UINT)Format < 256u && SampleCount <= 32u)
        InterlockedExchange64(&s_msaa12Cache[Format][SampleCount],
                              T12_QCACHE_VALID | (LONG64)n);
    return n;
}

VOID APIENTRY triton12GetMipPacking(D3D12DDI_HDEVICE, D3D12DDI_HRESOURCE,
                                    UINT *, UINT *); /* tritonResource12.c */

static VOID APIENTRY
triton12CheckFormatSupport(D3D12DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                           UINT *pOut)
{
    PTRITON12_DEVICE p = (PTRITON12_DEVICE)hDevice.pDrvPrivate;
    if (!pOut)
        return;
    *pOut = 0;
    if (!p || !p->pDev)
        return;
    if ((UINT)Format < 256u) {
        const LONG64 e = s_fmt12Cache[Format];
        if (e & T12_QCACHE_VALID) {
            *pOut = (UINT)e;
            return;
        }
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
    fs.Format = Format;
    fs.Support1 = 0;
    fs.Support2 = 0;
    HRESULT hr = ID3D12Device_CheckFeatureSupport(
        p->pDev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs));

    UINT caps = 0;
    if (SUCCEEDED(hr)) {
        /* Minimal accepted DDI bit set (see tritonTranslateFormatSupport:
         * forwarding the host's broad API-level set produced combinations
         * the runtime rejects; it infers the rest from feature level +
         * options caps). */
        if (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)
            caps |= D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
        if (fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)
            caps |= D3D12DDI_FORMAT_SUPPORT_RENDERTARGET;
        if (fs.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE)
            caps |= D3D12DDI_FORMAT_SUPPORT_BLENDABLE;
        if (fs.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER)
            caps |= D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
        if (!(caps & D3D12DDI_FORMAT_SUPPORT_RENDERTARGET))
            caps &= ~(UINT)D3D12DDI_FORMAT_SUPPORT_BLENDABLE;

        /* Bits the runtime grants per format only when the DDI sets them,
         * even though the feature itself is advertised in GetCaps:
         * typed UAV load/store behind TypedUAVLoadAdditionalFormats, and
         * logic ops behind OutputMergerLogicOp.  Left unset they
         * contradict the caps, and an app that consults both picks a
         * path the caps said it would not need. */
        if (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)
            caps |= D3D12DDI_FORMAT_SUPPORT_UAV_READS;
        if (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)
            caps |= D3D12DDI_FORMAT_SUPPORT_UAV_WRITES;
        if ((caps & D3D12DDI_FORMAT_SUPPORT_RENDERTARGET) &&
            (fs.Support2 & D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP))
            caps |= D3D12DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP;
        /* ld2dms on a multisampled view, including the X-padded depth
         * read formats, which no other bit here covers. */
        if (fs.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD)
            caps |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;

        /* Typed depth formats are not shader-sampleable or COLOUR render
         * targets at the DDI level. */
        switch (Format) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            caps &= ~(UINT)(D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE |
                            D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD |
                            D3D12DDI_FORMAT_SUPPORT_RENDERTARGET |
                            D3D12DDI_FORMAT_SUPPORT_BLENDABLE |
                            D3D12DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP);
            break;
        default:
            break;
        }

        /* MULTISAMPLE_RENDERTARGET must stay exactly consistent with
         * CheckMultisampleQualityLevels or the runtime throws during
         * finalization ("MSAA quality reported to be 0"). */
        if (t12MsaaQuality(p, Format, 4) > 0)
            caps |= D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
    } else if (Format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) {
        /* The only format allowed to carry the NOT_SUPPORTED bit. */
        caps = D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
    }

    *pOut = caps;
    /* Cache only host answers: a FAILED query is either a transport
     * fault (DEVICE_REMOVED) or the host's per-format rejection, and
     * the XR_BIAS NOT_SUPPORTED answer derived from the latter is cheap
     * to re-derive. */
    if (SUCCEEDED(hr) && (UINT)Format < 256u)
        InterlockedExchange64(&s_fmt12Cache[Format],
                              T12_QCACHE_VALID | (LONG64)caps);
}

static VOID APIENTRY
triton12CheckMultisampleQualityLevels(D3D12DDI_HDEVICE hDevice,
                                      DXGI_FORMAT Format,
                                      UINT SampleCount,
                                      D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS Flags,
                                      UINT *pNumQualityLevels)
{
    PTRITON12_DEVICE p = (PTRITON12_DEVICE)hDevice.pDrvPrivate;
    if (!pNumQualityLevels)
        return;
    *pNumQualityLevels = 0;
    if (!p || !p->pDev)
        return;
    if (Flags & D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAG_TILED_RESOURCE)
        return; /* no tiled resources */
    *pNumQualityLevels = t12MsaaQuality(p, Format, SampleCount);
}

static VOID APIENTRY
triton12QueryNodeMap(D3D12DDI_HDEVICE hDevice, UINT NumPhysicalAdapters,
                     UINT *pMap)
{
    (void)hDevice;
    TR_LOG("12.QueryNodeMap: n=%u", NumPhysicalAdapters);
    if (!pMap)
        return;
    /* Single physical adapter: identity map. */
    for (UINT i = 0; i < NumPhysicalAdapters; i++)
        pMap[i] = i;
}

/* ---------- extended features (table type 8): none supported ---------- */

static HRESULT APIENTRY
triton12GetSupportedExtendedFeatures(D3D12DDI_HDEVICE hDevice,
                                     UINT32 *puFeatures,
                                     D3D12DDI_FEATURE_0020 *pFeatures)
{
    (void)hDevice;
    (void)pFeatures;
    TR_LOG("12.GetSupportedExtendedFeatures: n=%u", puFeatures ? *puFeatures : 0);
    if (!puFeatures)
        return E_INVALIDARG;
    *puFeatures = 0; /* no extended features */
    return S_OK;
}

static HRESULT APIENTRY
triton12GetSupportedExtendedFeatureVersions(D3D12DDI_HDEVICE hDevice,
                                            D3D12DDI_FEATURE_0020 Feature,
                                            UINT32 *puFeatureVersions,
                                            UINT32 *pFeatureVersions)
{
    (void)hDevice;
    (void)pFeatureVersions;
    TR_LOG("12.GetSupportedExtendedFeatureVersions: feature=%d", (int)Feature);
    if (!puFeatureVersions)
        return E_INVALIDARG;
    *puFeatureVersions = 0;
    return S_OK;
}

static HRESULT APIENTRY
triton12EnableExtendedFeature(D3D12DDI_HDEVICE hDevice,
                              D3D12DDI_FEATURE_0020 Feature,
                              UINT32 FeatureVersion)
{
    (void)hDevice;
    TR_LOG("12.EnableExtendedFeature: feature=%d ver=%u", (int)Feature,
           FeatureVersion);
    return E_NOTIMPL; /* nothing to enable */
}

static HRESULT APIENTRY
triton12SetExtendedFeatureCallbacks(D3D12DDI_HDEVICE hDevice,
                                    D3D12DDI_TABLE_TYPE Table,
                                    const void *pTable,
                                    SIZE_T TableSize)
{
    (void)hDevice;
    (void)pTable;
    TR_LOG("12.SetExtendedFeatureCallbacks: table=%d size=%zu", (int)Table,
           TableSize);
    return S_OK;
}

static HRESULT APIENTRY
triton12GetOptionalDDITables(D3D12DDI_HADAPTER hAdapter,
                             UINT32 *puEntries,
                             D3D12DDI_TABLE_REQUEST *pTables)
{
    (void)hAdapter;
    TR_LOG("12.GetOptionalDDITables: puEntries=%p(%u) pTables=%p",
           (void *)puEntries, puEntries ? *puEntries : 0, (void *)pTables);
    if (!puEntries)
        return E_INVALIDARG;
    /* Request the DXGI table: without it,
     * CDXGISwapChain::InitializeSwapChainDeviceState throws E_INVALIDARG
     * and D3D12 swapchain creation fails. */
    if (!pTables) {
        *puEntries = 1;
        return S_OK;
    }
    if (*puEntries >= 1) {
        pTables[0].tableType = D3D12DDI_TABLE_TYPE_DXGI;
        pTables[0].numTables = 1;
        *puEntries = 1;
    } else {
        *puEntries = 0;
    }
    return S_OK;
}

static HRESULT APIENTRY
triton12FillDDITable(D3D12DDI_HADAPTER hAdapter,
                     D3D12DDI_TABLE_TYPE tableType,
                     VOID *pTable,
                     SIZE_T tableSize,
                     UINT tableNum,
                     D3D12DDI_HRTTABLE hRTTable)
{
    /* Log every table request (type + size names the runtime's expected
     * struct revision), fill with logging stubs, then overwrite the
     * implemented slots below. */
    TR_LOG("12.FillDDITable: type=%d size=%zu num=%u pTable=%p",
           (int)tableType, tableSize, tableNum, pTable);
    if (pTable && tableSize) {
        /* Stub every slot first, then overwrite the implemented ones, so
         * an unfilled slot is a named log line rather than a wild jump. */
        SIZE_T n = tableSize / sizeof(void *);
        if (n > 128)
            n = 128;
        memcpy(pTable, t12_stub_table, n * sizeof(void *));

        if (tableType == D3D12DDI_TABLE_TYPE_DEVICE_CORE &&
            tableSize >= sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0022)) {
            D3D12DDI_DEVICE_FUNCS_CORE_0022 *t =
                (D3D12DDI_DEVICE_FUNCS_CORE_0022 *)pTable;
            t->pfnCheckFormatSupport             = triton12CheckFormatSupport;
            t->pfnCheckMultisampleQualityLevels  = triton12CheckMultisampleQualityLevels;
            t->pfnGetMipPacking                  = triton12GetMipPacking;
            t->pfnQueryNodeMap                   = triton12QueryNodeMap;
            triton12InstallDescriptorFuncs(t);
            triton12InstallResourceFuncs(t);
            triton12InstallQueueDeviceFuncs(t);
            triton12InstallListDeviceFuncs(t);
            triton12InstallPipelineFuncs(t);
            triton12InstallPresentDeviceFuncs(t);
            triton12InstallQueryDeviceFuncs(t);
        }

        if (tableType == D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D &&
            tableSize >= sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0022)) {
            PTRITON12_ADAPTER pAdapter = (PTRITON12_ADAPTER)hAdapter.pDrvPrivate;
            if (pAdapter && tableNum < 2)
                pAdapter->hRTTableCmdList[tableNum] = hRTTable;
            triton12InstallListFuncs(
                (D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *)pTable);
            triton12InstallPresentFuncs(
                (D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *)pTable);
        }

        if (tableType == D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D &&
            tableSize >= sizeof(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001)) {
            triton12InstallQueueFuncs(
                (D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *)pTable);
        }

        if (tableType == D3D12DDI_TABLE_TYPE_0020_EXTENDED_FEATURES &&
            tableSize >= sizeof(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021)) {
            /* The runtime calls slot 0 immediately after the fill and
             * throws on a garbage out-count, so these need real bodies
             * even though no extended feature is supported. */
            D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 *t =
                (D3D12DDI_EXTENDED_FEATURES_FUNCS_0021 *)pTable;
            t->pfnGetSupportedExtendedFeatures = triton12GetSupportedExtendedFeatures;
            t->pfnGetSupportedExtendedFeatureVersions = triton12GetSupportedExtendedFeatureVersions;
            t->pfnEnableExtendedFeature = triton12EnableExtendedFeature;
            t->pfnSetExtendedFeatureCallbacks = triton12SetExtendedFeatureCallbacks;
        }
    }
    return S_OK;
}

/* ---------- OpenAdapter12 ---------- */

__declspec(dllexport)
HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER *pOpenData)
{
    TR_LOG("OpenAdapter12");

    /* Pins the D3D12 command queues to the primary ring (npt_device.h). */
    npt_d3d12_from_ddi = true;

    if (!pOpenData || !pOpenData->pAdapterFuncs)
        return E_INVALIDARG;

    PTRITON12_ADAPTER pAdapter = (PTRITON12_ADAPTER)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TRITON12_ADAPTER));
    if (!pAdapter)
        return E_OUTOFMEMORY;

    pAdapter->hRTAdapter  = pOpenData->hRTAdapter;

    /* Latch the host capset bits: every host-backend-dependent cap
     * answer (SM6/DXIL, FL 12_0) gates on them. */
    {
        struct triton_adapter_probe probe;
        tritonSharedBridgeAdapterProbe(&probe);
        pAdapter->HostCaps = probe.caps_flags;
        TR_LOG("OpenAdapter12: host caps=0x%08x", pAdapter->HostCaps);
    }

    pOpenData->hAdapter.pDrvPrivate = pAdapter;

    D3D12DDI_ADAPTERFUNCS *p = pOpenData->pAdapterFuncs;
    p->pfnCalcPrivateDeviceSize = triton12CalcPrivateDeviceSize;
    p->pfnCreateDevice          = triton12CreateDevice;
    p->pfnCloseAdapter          = triton12CloseAdapter;
    p->pfnGetSupportedVersions  = triton12GetSupportedVersions;
    p->pfnGetCaps               = triton12GetCaps;
    p->pfnGetOptionalDDITables  = triton12GetOptionalDDITables;
    p->pfnFillDDITable          = triton12FillDDITable;
    p->pfnDestroyDevice         = triton12DestroyDevice;
    return S_OK;
}
