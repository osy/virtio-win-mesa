/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Queries, predication, format / multisample / counter / direct-flip
 * checks, D3D11.1 Discard/ClearView, WDDM 1.3 markers, WDDM 2.0 Query
 * with ContextType.
 *
 * D3D10DDI_QUERY enum values match D3D11_QUERY 1:1 for everything we
 * expose: EVENT=0, OCCLUSION=1, TIMESTAMP=2, TIMESTAMP_DISJOINT=3,
 * PIPELINE_STATISTICS=4, OCCLUSION_PREDICATE=5, SO_STATISTICS=6,
 * SO_OVERFLOW_PREDICATE=7.
 */

#include "triton.h"
#include "triton_log.h"

static D3D11_QUERY tritonQueryDdiToD3D11(D3D10DDI_QUERY q)
{
    switch (q) {
    case D3D10DDI_QUERY_EVENT:                          return D3D11_QUERY_EVENT;
    case D3D10DDI_QUERY_OCCLUSION:                      return D3D11_QUERY_OCCLUSION;
    case D3D10DDI_QUERY_TIMESTAMP:                      return D3D11_QUERY_TIMESTAMP;
    case D3D10DDI_QUERY_TIMESTAMPDISJOINT:              return D3D11_QUERY_TIMESTAMP_DISJOINT;
    case D3D10DDI_QUERY_PIPELINESTATS:
    case D3D11DDI_QUERY_PIPELINESTATS:                  return D3D11_QUERY_PIPELINE_STATISTICS;
    case D3D10DDI_QUERY_OCCLUSIONPREDICATE:             return D3D11_QUERY_OCCLUSION_PREDICATE;
    case D3D10DDI_QUERY_STREAMOUTPUTSTATS:
    case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0:      return D3D11_QUERY_SO_STATISTICS_STREAM0;
    case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM1:      return D3D11_QUERY_SO_STATISTICS_STREAM1;
    case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM2:      return D3D11_QUERY_SO_STATISTICS_STREAM2;
    case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM3:      return D3D11_QUERY_SO_STATISTICS_STREAM3;
    case D3D10DDI_QUERY_STREAMOVERFLOWPREDICATE:
    case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM0: return D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0;
    case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM1: return D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1;
    case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM2: return D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2;
    case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3: return D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3;
    default: return D3D11_QUERY_EVENT;
    }
}

SIZE_T APIENTRY
tritonCalcPrivateQuerySize(D3D10DDI_HDEVICE hDev, const D3D10DDIARG_CREATEQUERY *pArgs)
{
    return sizeof(TRITON_QUERY);
}

void APIENTRY
tritonCreateQuery(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_CREATEQUERY *pArgs,
                  D3D10DDI_HQUERY hQuery, D3D10DDI_HRTQUERY hRTQuery)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_QUERY  q  = (PTRITON_QUERY)(hQuery.pDrvPrivate);
    if (!pD || !q) return;
    q->hRT      = hRTQuery;
    q->ddiKind  = pArgs->Query;
    q->pQuery   = NULL;

    D3D11_QUERY_DESC d = {};
    d.Query     = tritonQueryDdiToD3D11(pArgs->Query);
    d.MiscFlags = (pArgs->MiscFlags & D3D10DDI_QUERY_MISCFLAG_PREDICATEHINT)
                  ? D3D11_QUERY_MISC_PREDICATEHINT : 0;
    /* PREDICATEHINT means the runtime intends to use this with
     * SetPredication. CreatePredicate returns an ID3D11Predicate, which
     * derives from ID3D11Query, so the result fits the existing slot. */
    HRESULT hr;
    if (d.MiscFlags & D3D11_QUERY_MISC_PREDICATEHINT) {
        ID3D11Predicate *pPred = NULL;
        hr = ID3D11Device1_CreatePredicate(pD->pDev1, &d, &pPred);
        q->pQuery = (ID3D11Query *)pPred;
    } else {
        hr = ID3D11Device1_CreateQuery(pD->pDev1, &d, &q->pQuery);
    }
    if (FAILED(hr)) { TR_LOG("CreateQuery: 0x%08lx", hr); q->pQuery = NULL; }
}

void APIENTRY
tritonDestroyQuery(D3D10DDI_HDEVICE hDev, D3D10DDI_HQUERY hQuery)
{
    PTRITON_QUERY q = (PTRITON_QUERY)(hQuery.pDrvPrivate);
    if (!q) return;
    if (q->pQuery) { ID3D11Query_Release(q->pQuery); q->pQuery = NULL; }
}

void APIENTRY
tritonQueryBegin(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY hQuery)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_QUERY  q  = (PTRITON_QUERY)(hQuery.pDrvPrivate);
    if (!pD || !q || !q->pQuery) return;
    ID3D11DeviceContext1_Begin(pD->pCtx1, (ID3D11Asynchronous *)q->pQuery);
}

void APIENTRY
tritonQueryEnd(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY hQuery)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_QUERY  q  = (PTRITON_QUERY)(hQuery.pDrvPrivate);
    if (!pD || !q || !q->pQuery) return;
    ID3D11DeviceContext1_End(pD->pCtx1, (ID3D11Asynchronous *)q->pQuery);
}

void APIENTRY
tritonQueryGetData(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY hQuery,
                   VOID *pData, UINT DataSize, UINT Flags)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_QUERY  q  = (PTRITON_QUERY)(hQuery.pDrvPrivate);
    if (!pD || !q || !q->pQuery) return;
    /* DDI's D3D10_DDI_GET_DATA_FLAG values match D3D11_ASYNC_GETDATA_DONOTFLUSH.
     * GetData returns S_FALSE when the query result isn't ready. The DDI's
     * QueryGetData is VOID, so we propagate that via pfnSetErrorCb so the
     * app doesn't read stale pData. */
    HRESULT hr = ID3D11DeviceContext1_GetData(pD->pCtx1, (ID3D11Asynchronous *)q->pQuery,
                                              pData, DataSize, Flags);
    if (hr == S_FALSE)
        tritonSetError(pD, DXGI_DDI_ERR_WASSTILLDRAWING);
    else if (FAILED(hr))
        tritonSetError(pD, hr);
}

void APIENTRY
tritonSetPredication(D3D10DDI_HDEVICE hDevice, D3D10DDI_HQUERY hPred, BOOL PredicateValue)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_QUERY  q  = hPred.pDrvPrivate ? (PTRITON_QUERY)(hPred.pDrvPrivate) : NULL;
    if (!pD) return;
    ID3D11DeviceContext1_SetPredication(pD->pCtx1,
        q ? (ID3D11Predicate *)q->pQuery : NULL, PredicateValue);
}

/* ---------- Format / multisample / counter checks ---------- */

/* The DDI's *pFormatCaps is the D3D10_DDI_FORMAT_SUPPORT_* /
 * D3D11_1DDI_FORMAT_SUPPORT_* / D3DWDDM*_* bitfield, which is a DIFFERENT
 * layout from the public D3D11_FORMAT_SUPPORT(2) API bits (e.g. SHADER
 * sample is DDI 0x1 vs API 0x200; RENDER_TARGET is DDI 0x2 vs API
 * 0x4000). It reports the optional, format-dependent caps the runtime
 * can't infer from the feature level, and it merges what the API splits
 * across FORMAT_SUPPORT and FORMAT_SUPPORT2. Translate bit-by-bit (cf.
 * VBox vboxDXCheckFormatSupport, which fills the same DDI bits from SVGA
 * caps). Unmapped DDI bits (CAPTURE, MULTIPLANE_OVERLAY, TILED,
 * DISPLAYABLE) have no CheckFormatSupport API equivalent and stay 0. */
/* Single source of truth for multisample support, shared by
 * CheckMultisampleQualityLevels AND the CheckFormatSupport
 * MULTISAMPLE_RENDERTARGET bit, so the two can never disagree -- the D3D11
 * runtime rejects any disagreement during device finalization with "MSAA
 * quality reported to be 0" -> DXGI_ERROR_DRIVER_INTERNAL_ERROR.  Also
 * enforces the typeless<->typed invariant across ALL typeless families: dxvk
 * maps a family's typeless parent and typed views to different Vulkan formats
 * and can report different MSAA per member (typed depth vs padded read-view;
 * typed R32G32B32_FLOAT vs its typeless parent), which the runtime rejects as
 * "MSAA quality for parent != child".  tritonMsaaQuality resolves every
 * format to its family and reports a conservative family value to the
 * typeless parent and participating targets.  Depth read-only views remain
 * zero because the runtime requires formats without the MSAA-target cap to
 * report no multisample quality levels. */
/* Cap-query results are constant per host device, but DWM/Direct2D re-query
 * format caps on every composition frame, and each query is a host round-trip
 * whose reply-wait sleeps the calling (compositor) thread.  Answer re-queries
 * from a local cache; the values are idempotent, so the lock-free cache is
 * benign-race-safe. */
static UINT          s_msaaCache[256][33];
static unsigned char s_msaaValid[256][33];
static UINT          s_fmtCache[256];
static unsigned char s_fmtValid[256];

/* Every DXGI typeless format family: the typeless parent together with all
 * its typed view formats (depth, colour, integer, sRGB).  MSAA is a
 * resource-allocation property shared by the whole family, but dxvk-over-
 * Vulkan maps each member to a DIFFERENT Vulkan format and can report a
 * different MSAA quality per member (e.g. the typed R32G32B32_FLOAT view
 * supports 2x while the R32G32B32_TYPELESS parent reports 0; the typed depth
 * member of R32G8X24 is MSAA-capable while its padded read-only views are
 * not).  The D3D11 runtime requires the typeless parent and participating
 * targets to agree, while read-only depth views must report zero when their
 * concrete format lacks the MSAA-target cap.  Rows shorter than the width
 * are zero-filled by C, and DXGI_FORMAT_UNKNOWN (0) terminates each row. */
static const DXGI_FORMAT k_msaaGroups[][7] = {
    { DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_SINT },
    { DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32_SINT },
    { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SINT },
    { DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_SINT },
    { DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT },
    { DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UINT, DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM },
    { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_SINT },
    { DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_SINT },
    { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT },
    { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_X24_TYPELESS_G8_UINT },
    { DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_SINT },
    { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_SINT },
    { DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_SINT },
    { DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB },
    { DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB },
    { DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB },
    { DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_SNORM },
    { DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_SNORM },
    { DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB },
    { DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB },
    { DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_SF16 },
    { DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB },
};

/* Locate the family row containing Format, or NULL for a standalone format
 * (R11G11B10_FLOAT, A8_UNORM, packed/video formats) that has no typeless
 * family and thus no parent==child constraint. */
static const DXGI_FORMAT *tritonMsaaGroup(DXGI_FORMAT Format)
{
    if (Format == DXGI_FORMAT_UNKNOWN)
        return NULL;
    for (unsigned i = 0; i < sizeof(k_msaaGroups) / sizeof(k_msaaGroups[0]); i++)
        for (unsigned j = 0; j < 7 && k_msaaGroups[i][j] != DXGI_FORMAT_UNKNOWN; j++)
            if (k_msaaGroups[i][j] == Format)
                return k_msaaGroups[i];
    return NULL;
}

static BOOL tritonMsaaReadOnlyDepthView(DXGI_FORMAT Format)
{
    switch (Format) {
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL tritonMsaaForbidden(DXGI_FORMAT Format)
{
    switch (Format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC7_TYPELESS:
        return TRUE;
    default:
        return FALSE;
    }
}

static UINT tritonNativeMsaaQuality(PTRITON_DEVICE pD, DXGI_FORMAT Format,
                                    UINT SampleCount)
{
    UINT support = 0;
    UINT quality = 0;
    if (FAILED(ID3D11Device1_CheckFormatSupport(pD->pDev1, Format, &support)) ||
        !(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) ||
        FAILED(ID3D11Device1_CheckMultisampleQualityLevels(pD->pDev1, Format,
                                                            SampleCount, &quality)))
        return 0;
    return quality;
}

static UINT tritonMsaaQuality(PTRITON_DEVICE pD, DXGI_FORMAT Format, UINT SampleCount)
{
    if ((UINT)Format < 256u && SampleCount <= 32u && s_msaaValid[Format][SampleCount])
        return s_msaaCache[Format][SampleCount];

    /* Required DDI edge cases, independent of format capability. */
    if (SampleCount == 1)
        return 1;
    if (SampleCount == 0 || SampleCount > 32)
        return 0;

    /* The runtime compares the optional MSAA-render-target capability across
     * participating typed members of a colour cast set.  Use the conservative
     * minimum of the members' native per-format results, so the whole group is
     * enabled only when every member can satisfy the DDI contract.  Depth
     * targets are singleton groups; their typeless/read-only views are handled
     * by the forbidden path above. */
    const DXGI_FORMAT *group = tritonMsaaGroup(Format);
    if (group) {
        UINT n = ~0u;
        for (unsigned j = 0; j < 7 && group[j] != DXGI_FORMAT_UNKNOWN; j++) {
            /* Typeless parents and depth read-only views are not native
             * render-target candidates.  Derive the family value from the
             * legal typed targets. */
            if (tritonMsaaForbidden(group[j]))
                continue;
            UINT m = tritonNativeMsaaQuality(pD, group[j], SampleCount);
            if (m < n)
                n = m;
        }
        if (n == ~0u)
            n = 0;
        if (SampleCount <= 32u)
            for (unsigned j = 0; j < 7 && group[j] != DXGI_FORMAT_UNKNOWN; j++)
                if ((UINT)group[j] < 256u) {
                    /* Typeless parents inherit the family value for the
                     * runtime's parent/child comparison.  Read-only depth
                     * views cannot be MSAA targets and must report zero. */
                    s_msaaCache[group[j]][SampleCount] =
                        tritonMsaaReadOnlyDepthView(group[j]) ? 0 : n;
                    s_msaaValid[group[j]][SampleCount] = 1;
                }
        return tritonMsaaReadOnlyDepthView(Format) ? 0 : n;
    }

    UINT n = tritonNativeMsaaQuality(pD, Format, SampleCount);
    if ((UINT)Format < 256u && SampleCount <= 32u) {
        s_msaaCache[Format][SampleCount] = n; s_msaaValid[Format][SampleCount] = 1;
    }
    return n;
}

static UINT tritonTranslateFormatSupport(UINT s1, UINT s2)
{
    UINT caps = 0;
    /*
     * Report only the minimal DDI bit set that VBox's vboxDXCheckFormatSupport
     * reports and the D3D11 runtime is known to accept.  The host (dxvk over
     * Vulkan) reports a much broader API-level set -- MULTISAMPLE_LOAD,
     * SHADER_GATHER, VIDEO_PROCESSOR_*, BUFFER, UAV, DISPLAY, DECODER -- and
     * forwarding all of it produced DDI combinations the runtime rejects at
     * D3D11CreateDevice with DXGI_ERROR_DRIVER_INTERNAL_ERROR (0x887a0020),
     * notably for depth/typeless formats: the API MULTISAMPLE/RENDER_TARGET
     * bits there denote depth targets, but the matching DDI bits denote COLOR
     * render targets, so a 1:1 forward is invalid.  The runtime infers the
     * omitted capabilities (gather, typed UAV load/store, buffer views, etc.)
     * from the feature level + the D3D11_OPTIONS caps, not per-format here.
     */
    if (s1 & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)             caps |= D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
    if (s1 & D3D11_FORMAT_SUPPORT_RENDER_TARGET)            caps |= D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
    if (s1 & D3D11_FORMAT_SUPPORT_BLENDABLE)                caps |= D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;
    if (s1 & D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER)         caps |= D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
    /* MULTISAMPLE_RENDERTARGET is intentionally NOT mapped here -- the caller
     * (tritonCheckFormatSupport) sets it from tritonMsaaQuality() so it stays
     * exactly consistent with CheckMultisampleQualityLevels. */

    /* BLENDABLE requires a colour RENDERTARGET, so drop it when RENDERTARGET
     * is absent. */
    if (!(caps & D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET))
        caps &= ~(UINT)D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;
    (void)s2;
    return caps;
}

void APIENTRY
tritonCheckFormatSupport(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format, UINT *pOut)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD || !pOut) { if (pOut) *pOut = 0; return; }
    *pOut = 0;
    if ((UINT)Format < 256u && s_fmtValid[Format]) { *pOut = s_fmtCache[Format]; return; }
    UINT s1 = 0;
    if (FAILED(ID3D11Device1_CheckFormatSupport(pD->pDev1, Format, &s1)))
        return;
    /* FORMAT_SUPPORT2 (typed UAV load/store, OM logic op) comes from a
     * separate feature query; absence is non-fatal (leaves those 0). */
    UINT s2 = 0;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs2 = { Format, 0 };
    if (SUCCEEDED(ID3D11Device1_CheckFeatureSupport(pD->pDev1,
            D3D11_FEATURE_FORMAT_SUPPORT2, &fs2, sizeof(fs2))))
        s2 = fs2.OutFormatSupport2;
    *pOut = tritonTranslateFormatSupport(s1, s2);

    /* Typed depth-stencil formats are not shader-sampleable or COLOUR render
     * targets in D3D (you sample/colour-target through the typeless/typed
     * view formats), but dxvk-over-Vulkan reports SHADER_SAMPLE/RENDERTARGET
     * for them because the underlying Vulkan format supports it.  Clear those
     * colour caps.  Do NOT clear MULTISAMPLE_RENDERTARGET: it is the depth
     * format's MSAA-depth advertisement, and it must stay consistent with
     * CheckMultisampleQualityLevels (which reports MSAA for these formats) or
     * the runtime throws "MSAA quality reported to be 0" ->
     * DXGI_ERROR_DRIVER_INTERNAL_ERROR during device finalization. */
    switch (Format) {
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        *pOut &= ~(UINT)(D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE |
                         D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET |
                         D3D10_DDI_FORMAT_SUPPORT_BLENDABLE);
        break;
    default:
        break;
    }

    /* Keep the quality value uniform across a typeless family, but do not let
     * a member inherit MULTISAMPLE_RENDERTARGET eligibility from another
     * member.  The D3D11 runtime validates this DDI bit against a per-format
     * requirements table: advertising it for R32G32_UINT merely because a
     * sibling in the R32G32 family has non-zero MSAA quality causes a direct
     * DXGI_ERROR_DRIVER_INTERNAL_ERROR during device finalization.
     *
     * The host CheckFormatSupport result is the authority for whether this
     * concrete format may be an MSAA render target; tritonMsaaQuality remains
     * the authority for the family-consistent quality count.  Both conditions
     * must hold.  A 4x query is the FL10.1+ quality gate. */
    if (!tritonMsaaForbidden(Format) && tritonMsaaQuality(pD, Format, 4) > 0)
        *pOut |= D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
    else
        *pOut &= ~(UINT)D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
    if ((UINT)Format < 256u) { s_fmtCache[Format] = *pOut; s_fmtValid[Format] = 1; }
}

void APIENTRY
tritonCheckMultisampleQualityLevels(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                                    UINT SampleCount, UINT *pNumQualityLevels)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD || !pNumQualityLevels) { if (pNumQualityLevels) *pNumQualityLevels = 0; return; }
    *pNumQualityLevels = tritonMsaaQuality(pD, Format, SampleCount);
}

/* CheckCounter* are vendor-specific; no counters exposed. */
void APIENTRY
tritonCheckCounterInfo(D3D10DDI_HDEVICE hDev, D3D10DDI_COUNTER_INFO *pInfo)
{
    if (!pInfo) return;
    pInfo->LastDeviceDependentCounter   = (D3D10DDI_QUERY)0;
    pInfo->NumSimultaneousCounters      = 0;
    pInfo->NumDetectableParallelUnits   = 0;
}

void APIENTRY
tritonCheckCounter(D3D10DDI_HDEVICE hDev, D3D10DDI_QUERY q, D3D10DDI_COUNTER_TYPE *pType,
                   UINT *pActiveCounters,
                   LPSTR pName,             UINT *pNameLength,
                   LPSTR pUnits,            UINT *pUnitsLength,
                   LPSTR pDescription,      UINT *pDescriptionLength)
{
    if (pType)              *pType = D3D10DDI_COUNTER_TYPE_UINT32;
    if (pActiveCounters)    *pActiveCounters = 0;
    if (pNameLength)        *pNameLength = 0;
    if (pUnitsLength)       *pUnitsLength = 0;
    if (pDescriptionLength) *pDescriptionLength = 0;
    (void)pName; (void)pUnits; (void)pDescription;
}

/* ---------- D3D11.1 Discard + ClearView ---------- */

void APIENTRY
tritonDiscard(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE HandleType,
              VOID *hResourceOrView, const D3D10_DDI_RECT *pRects, UINT NumRects)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD || !hResourceOrView) return;
    /* D3D11.1 DiscardView/Resource ignore the rect array. */
    (void)pRects; (void)NumRects;
    if (HandleType == D3D10DDI_HT_RESOURCE) {
        PTRITON_RESOURCE r = (PTRITON_RESOURCE)(hResourceOrView);
        if (r && r->pResource)
            ID3D11DeviceContext1_DiscardResource(pD->pCtx1, r->pResource);
    } else {
        switch (HandleType) {
        case D3D10DDI_HT_SHADERRESOURCEVIEW: {
            PTRITON_SRVIEW v = (PTRITON_SRVIEW)(hResourceOrView);
            if (v && v->pSRV) ID3D11DeviceContext1_DiscardView(pD->pCtx1, (ID3D11View *)v->pSRV);
            break;
        }
        case D3D10DDI_HT_RENDERTARGETVIEW: {
            PTRITON_RTVIEW v = (PTRITON_RTVIEW)(hResourceOrView);
            if (v && v->pRTV) ID3D11DeviceContext1_DiscardView(pD->pCtx1, (ID3D11View *)v->pRTV);
            break;
        }
        case D3D10DDI_HT_DEPTHSTENCILVIEW: {
            PTRITON_DSVIEW v = (PTRITON_DSVIEW)(hResourceOrView);
            if (v && v->pDSV) ID3D11DeviceContext1_DiscardView(pD->pCtx1, (ID3D11View *)v->pDSV);
            break;
        }
        case D3D11DDI_HT_UNORDEREDACCESSVIEW: {
            PTRITON_UAVIEW v = (PTRITON_UAVIEW)(hResourceOrView);
            if (v && v->pUAV) ID3D11DeviceContext1_DiscardView(pD->pCtx1, (ID3D11View *)v->pUAV);
            break;
        }
        default: break;
        }
    }
}

void APIENTRY
tritonClearView(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE HandleType,
                VOID *hView, const FLOAT Color[4],
                const D3D10_DDI_RECT *pRect, UINT NumRects)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD || !hView) return;
    /* PFND3D11_1DDI_CLEARVIEW: valid types are RTV, UAV, and the video
     * view family. DSV uses pfnClearDepthStencilView instead. */
    ID3D11View *pView = NULL;
    switch (HandleType) {
    case D3D10DDI_HT_RENDERTARGETVIEW: {
        PTRITON_RTVIEW v = (PTRITON_RTVIEW)(hView);
        pView = v ? (ID3D11View *)v->pRTV : NULL; break;
    }
    case D3D11DDI_HT_UNORDEREDACCESSVIEW: {
        PTRITON_UAVIEW v = (PTRITON_UAVIEW)(hView);
        pView = v ? (ID3D11View *)v->pUAV : NULL; break;
    }
    default: break;
    }
    if (pView) {
        ID3D11DeviceContext1_ClearView(pD->pCtx1, pView, Color,
                                       (const D3D11_RECT *)(pRect), NumRects);
    }
}

void APIENTRY
tritonCheckDirectFlipSupport(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes, D3D10DDI_HRESOURCE hRes2,
                             UINT flags, BOOL *pSupported)
{
    if (pSupported) *pSupported = FALSE;
}

void APIENTRY
tritonAssignDebugBinary(D3D10DDI_HDEVICE hDev, D3D10DDI_HSHADER hShader, UINT size, const VOID *pData) {}

/* WDDM 1.3 CheckMultisampleQualityLevels (Flags-aware). The DDI's
 * D3DWDDM1_3DDI_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE bit
 * matches D3D11's, so pass through. Falls back without Device3. */

void APIENTRY
tritonCheckMultisampleQualityLevels_1_3(D3D10DDI_HDEVICE hDevice, DXGI_FORMAT Format,
                                        UINT SampleCount, UINT Flags,
                                        UINT *pNumQualityLevels)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD || !pNumQualityLevels) { if (pNumQualityLevels) *pNumQualityLevels = 0; return; }

    /* The ordinary WDDM 1.3 query has the same contract as the legacy DDI.
     * Keep it on the family-normalized path used by CheckFormatSupport;
     * otherwise Device3 exposes the per-Vulkan-format Host result again and
     * reintroduces both parent/child mismatches and support-vs-quality zero.
     * Only the tiled-resource variant has semantics the legacy helper cannot
     * represent and therefore needs the Flags-aware Device3 query. */
    if (Flags == 0) {
        tritonCheckMultisampleQualityLevels(hDevice, Format, SampleCount,
                                            pNumQualityLevels);
        return;
    }

    if (pD->pDev3) {
        UINT n = 0;
        HRESULT hr = ID3D11Device3_CheckMultisampleQualityLevels1(
            pD->pDev3, Format, SampleCount, Flags, &n);
        *pNumQualityLevels = SUCCEEDED(hr) ? n : 0;
        return;
    }
    TR_STUB("CheckMultisampleQualityLevels_1_3 with Flags (no Device3)");
    *pNumQualityLevels = 0;
}

/* WDDM 1.3 profiling markers. The DDI provides no name string with
 * pfnSetMarker and signals mode via pfnSetMarkerMode; the public D3D11
 * marker API (ID3DUserDefinedAnnotation) requires LPCWSTR names and has
 * no mode concept, so DDI semantics can't be reconstructed on top of it. */

void APIENTRY
tritonSetMarker(D3D10DDI_HDEVICE hDev)
{
    TR_STUB("SetMarker (DDI has no name; D3D11 marker API requires one)");
}

void APIENTRY
tritonSetMarkerMode(D3D10DDI_HDEVICE hDev, D3DWDDM1_3DDI_MARKER_TYPE type, UINT flags)
{
    TR_STUB("SetMarkerMode (driver-internal; no D3D11 equivalent)");
}

/* WDDM 2.0 Query / Flush with ContextType.
 *
 * The DDI's D3DWDDM2_0DDI_CONTEXTTYPE_FLAG is a BIT flag (ALL=0, 3D=1,
 * COMPUTE=2, COPY=4, VIDEO=8); D3D11_CONTEXT_TYPE is a SEQUENTIAL enum
 * (ALL=0, 3D=1, COMPUTE=2, COPY=3, VIDEO=4). They diverge at COPY/VIDEO,
 * so a raw cast mis-maps them — convert explicitly. ID3D11Query1 derives
 * from ID3D11Query so the result fits the existing TRITON_QUERY slot.
 * Falls back to the 11.x path without Device3 (ContextType pinned to 3D).
 * Non-static: shared with tritonFlush_WDDM2_0 in tritonView.c. */

D3D11_CONTEXT_TYPE tritonContextFlagToEnum(UINT flag)
{
    switch (flag) {
    case 0:                                          return D3D11_CONTEXT_TYPE_ALL;
    case 1 /* D3DWDDM2_0DDI_CONTEXTTYPE_3D */:       return D3D11_CONTEXT_TYPE_3D;
    case 2 /* D3DWDDM2_0DDI_CONTEXTTYPE_COMPUTE */:  return D3D11_CONTEXT_TYPE_COMPUTE;
    case 4 /* D3DWDDM2_0DDI_CONTEXTTYPE_COPY */:     return D3D11_CONTEXT_TYPE_COPY;
    case 8 /* D3DWDDM2_0DDI_CONTEXTTYPE_VIDEO */:    return D3D11_CONTEXT_TYPE_VIDEO;
    default:                                         return D3D11_CONTEXT_TYPE_ALL;
    }
}

SIZE_T APIENTRY
tritonCalcPrivateQuerySize_WDDM2_0(D3D10DDI_HDEVICE hDev, const D3DWDDM2_0DDIARG_CREATEQUERY *pArgs)
{
    return sizeof(TRITON_QUERY);
}

void APIENTRY
tritonCreateQuery_WDDM2_0(D3D10DDI_HDEVICE hDevice,
                          const D3DWDDM2_0DDIARG_CREATEQUERY *pArgs,
                          D3D10DDI_HQUERY hQuery, D3D10DDI_HRTQUERY hRTQuery)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_QUERY  q  = (PTRITON_QUERY)(hQuery.pDrvPrivate);
    if (!pD || !q) return;

    if (!pD->pDev3) {
        D3D10DDIARG_CREATEQUERY a;
        a.Query     = pArgs->Query;
        a.MiscFlags = pArgs->MiscFlags;
        tritonCreateQuery(hDevice, &a, hQuery, hRTQuery);
        if (pArgs->ContextType != 0 && pArgs->ContextType != 1)
            TR_LOG("CreateQuery_WDDM2_0: non-3D ContextType=%u without Device3", pArgs->ContextType);
        return;
    }

    q->hRT     = hRTQuery;
    q->ddiKind = pArgs->Query;
    q->pQuery  = NULL;

    D3D11_QUERY_DESC1 d = {};
    d.Query       = tritonQueryDdiToD3D11(pArgs->Query);
    d.MiscFlags   = (pArgs->MiscFlags & D3D10DDI_QUERY_MISCFLAG_PREDICATEHINT)
                    ? D3D11_QUERY_MISC_PREDICATEHINT : 0;
    d.ContextType = tritonContextFlagToEnum(pArgs->ContextType);

    HRESULT hr;
    if (d.MiscFlags & D3D11_QUERY_MISC_PREDICATEHINT) {
        /* No CreatePredicate1 with ContextType in D3D11.3; fall back to
         * the 11.0 path (ContextType discarded for predicates). */
        D3D11_QUERY_DESC d0 = { d.Query, d.MiscFlags };
        ID3D11Predicate *pPred = NULL;
        hr = ID3D11Device1_CreatePredicate(pD->pDev1, &d0, &pPred);
        q->pQuery = (ID3D11Query *)pPred;
    } else {
        ID3D11Query1 *pQuery1 = NULL;
        hr = ID3D11Device3_CreateQuery1(pD->pDev3, &d, &pQuery1);
        q->pQuery = (ID3D11Query *)pQuery1;
    }
    if (FAILED(hr)) { TR_LOG("CreateQuery_WDDM2_0: 0x%08lx", hr); q->pQuery = NULL; }
}
