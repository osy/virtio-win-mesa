/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shader create / destroy / set, GS-with-stream-output, and lazy
 * ID3D11InputLayout resolution against the bound VS.
 *
 * The DDI passes pShaderCode as a tokenized program (pShaderCode[0] is
 * the version token, pShaderCode[1] is length in DWORDs). D3D11's
 * CreateXxxShader API expects a DXBC container, so we wrap via
 * tritonBuildDxbc before forwarding.
 */

#include "triton.h"
#include "triton_log.h"
#include "npt_workaround.h"  /* npt_host_workaround_flags() + NPT_WA_* bits */
#include "tritonDxbc.h"

/* Declared early so tritonVsSetShader can call it. */
BOOL tritonResolveInputLayout(PTRITON_DEVICE pD);

static SIZE_T tritonShaderTokenBytes(const UINT *pShaderCode)
{
    return (SIZE_T)pShaderCode[1] * sizeof(UINT);
}

/* Choose the DDI signature-entry stride based on interface version: 10.0
 * and 11.0 tables pass D3D10DDIARG_SIGNATURE_ENTRY (12 B); the 11.1 table
 * passes D3D11_1DDIARG_SIGNATURE_ENTRY (20 B, adds RegisterComponentType
 * + MinPrecision). */
static UINT tritonSigStride(const PTRITON_DEVICE pD)
{
    return (pD && pD->uIfVersion >= D3D11_1_DDI_INTERFACE_VERSION) ? 20u : 12u;
}

static HRESULT tritonShaderCreateNative(PTRITON_DEVICE pD, TRITON_SHADER_KIND k,
                                        const void *p, SIZE_T cb,
                                        ID3D11DeviceChild **out)
{
    switch (k) {
    case TRITON_SHADER_VS: return ID3D11Device1_CreateVertexShader(
                                pD->pDev1, p, cb, NULL, (ID3D11VertexShader **)out);
    case TRITON_SHADER_PS: return ID3D11Device1_CreatePixelShader(
                                pD->pDev1, p, cb, NULL, (ID3D11PixelShader **)out);
    case TRITON_SHADER_GS: return ID3D11Device1_CreateGeometryShader(
                                pD->pDev1, p, cb, NULL, (ID3D11GeometryShader **)out);
    case TRITON_SHADER_HS: return ID3D11Device1_CreateHullShader(
                                pD->pDev1, p, cb, NULL, (ID3D11HullShader **)out);
    case TRITON_SHADER_DS: return ID3D11Device1_CreateDomainShader(
                                pD->pDev1, p, cb, NULL, (ID3D11DomainShader **)out);
    case TRITON_SHADER_CS: return ID3D11Device1_CreateComputeShader(
                                pD->pDev1, p, cb, NULL, (ID3D11ComputeShader **)out);
    }
    return E_INVALIDARG;
}

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateShaderSize, TRITON_SHADER,
                     const UINT *pTokens, const VOID *pArgs)

/* Tessellation shader size — same as regular shader size. The DDI hands
 * tessellation IO signatures separately but we forward DXBC unchanged.
 * Must return non-zero: a 0-sized allocation would cause the create
 * handler to write into a 0-byte buffer. */
TR_CALC_PRIVATE_SIZE(tritonCalcPrivateTessellationShaderSize, TRITON_SHADER,
                     const UINT *pTokens, const VOID *pArgs)

/* Unpack a stage-IO or tessellation-IO signatures struct from a raw
 * void*. D3D10DDIARG_STAGE_IO_SIGNATURES and
 * D3D11_1DDIARG_STAGE_IO_SIGNATURES share the same head layout -- 2
 * (ptr, count) pairs. The tessellation variant tacks on a third pair for
 * patch-constant signatures.
 *
 * The pair stride is POINTER-SIZE dependent: {const void *; UINT} pads to
 * pointer alignment, so a pair is 16 B on x64/arm64 but 8 B on x86, and the
 * count sits at +8 vs +4. Deriving the offsets from sizeof(void *) reproduces
 * the 64-bit layout byte for byte while making the 32-bit (WOW64) UMD read the
 * right fields. */
typedef struct TritonStageSigs {
    const void *pIn;     UINT cIn;
    const void *pOut;    UINT cOut;
    const void *pPatch;  UINT cPatch;
} TritonStageSigs;

static void tritonUnpackSigs(const void *pRaw, BOOL fTess, TritonStageSigs *out)
{
    out->pIn = NULL;    out->cIn = 0;
    out->pOut = NULL;   out->cOut = 0;
    out->pPatch = NULL; out->cPatch = 0;
    if (!pRaw) return;
    const BYTE *p = (const BYTE *)(pRaw);
    const SIZE_T cbPtr  = sizeof(void *);      /* 8 on x64/arm64, 4 on x86 */
    const SIZE_T cbPair = 2 * cbPtr;           /* ptr + UINT + tail padding  */
    out->pIn  = *(const void *const *)(p);
    out->cIn  = *(const UINT *)(p + cbPtr);
    out->pOut = *(const void *const *)(p + cbPair);
    out->cOut = *(const UINT *)(p + cbPair + cbPtr);
    if (fTess) {
        out->pPatch = *(const void *const *)(p + 2 * cbPair);
        out->cPatch = *(const UINT *)(p + 2 * cbPair + cbPtr);
    }
}

/* Only a VS (input-layout resolution, stream-output-only GS source), a DS
 * (stream-output-only GS source) and a PS reading vCoverage (its
 * target-independent-rasterization copy) read their DXBC after create. */
static void tritonShaderDropUnusedBytecode(PTRITON_SHADER s)
{
    if (s->kind == TRITON_SHADER_VS || s->kind == TRITON_SHADER_DS ||
        s->fReadsCoverage)
        return;
    if (s->pBytecode)
        HeapFree(GetProcessHeap(), 0, s->pBytecode);
    s->pBytecode  = NULL;
    s->cbBytecode = 0;
}

static void tritonCreateShaderCommon(D3D10DDI_HDEVICE hDevice, TRITON_SHADER_KIND kind,
                                     const UINT *pShaderCode,
                                     D3D10DDI_HSHADER hShader,
                                     D3D10DDI_HRTSHADER hRTShader,
                                     const void *pSigsRaw)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD || !s) return;
    s->kind           = kind;
    s->pBytecode      = NULL;
    s->cbBytecode     = 0;
    s->u.pDeviceChild = NULL;
    s->cookie         = ++pD->nextShaderCookie;
    s->fSoOnly        = FALSE;
    s->pSoDecl        = NULL;
    s->cSoDecl        = 0;
    s->pSoOnlyGS      = NULL;
    s->fReadsCoverage = FALSE;
    memset(s->apTirPS, 0, sizeof(s->apTirPS));
    s->TirTried       = 0;

    const SIZE_T cbTokens = tritonShaderTokenBytes(pShaderCode);
    /* Compute shaders have no input/output signature; HS/DS carry an
     * additional patch-constant signature. */
    const BOOL fTess = (kind == TRITON_SHADER_HS || kind == TRITON_SHADER_DS);
    TritonStageSigs sigs;
    tritonUnpackSigs(pSigsRaw, fTess, &sigs);

    if (sigs.cIn > 4096 || sigs.cOut > 4096 || sigs.cPatch > 4096) {
        TR_LOG("CreateShader[%d]: signature counts exceed 4096 (in=%u out=%u patch=%u)",
               (int)kind, sigs.cIn, sigs.cOut, sigs.cPatch);
        tritonSetCreateError(pD, E_INVALIDARG);
        return;
    }

    SIZE_T cbDxbc = 0;
    void *pDxbc = tritonBuildDxbc(pShaderCode, cbTokens,
                                   sigs.pIn,    sigs.cIn,
                                   sigs.pOut,   sigs.cOut,
                                   sigs.pPatch, sigs.cPatch,
                                   tritonSigStride(pD),
                                   &cbDxbc);
    if (!pDxbc) {
        TR_LOG("CreateShader[%d]: tritonBuildDxbc failed", (int)kind);
        tritonSetCreateError(pD, E_OUTOFMEMORY);
        return;
    }
    s->pBytecode  = pDxbc;
    s->cbBytecode = cbDxbc;

    HRESULT hr = tritonShaderCreateNative(pD, kind, s->pBytecode, s->cbBytecode,
                                          &s->u.pDeviceChild);
    if (FAILED(hr)) {
        TR_LOG("CreateShader[%d]: D3D11 create failed 0x%08lx", (int)kind, hr);
        s->u.pDeviceChild = NULL;
        /* The runtime does not call pfnDestroyShader after a create error,
         * so free the DXBC container now or it leaks. */
        HeapFree(GetProcessHeap(), 0, s->pBytecode);
        s->pBytecode  = NULL;
        s->cbBytecode = 0;
        tritonSetCreateError(pD, hr);
        return;
    }
    s->fReadsCoverage = tritonDxbcReadsInputCoverage(pShaderCode, cbTokens);
    tritonShaderDropUnusedBytecode(s);
}

void APIENTRY
tritonCreateVertexShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                         D3D10DDI_HSHADER h, D3D10DDI_HRTSHADER hRT, const VOID *pSigs)
{
    tritonCreateShaderCommon(hDevice, TRITON_SHADER_VS, pCode, h, hRT, pSigs);
}
void APIENTRY
tritonCreatePixelShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                        D3D10DDI_HSHADER h, D3D10DDI_HRTSHADER hRT, const VOID *pSigs)
{
    tritonCreateShaderCommon(hDevice, TRITON_SHADER_PS, pCode, h, hRT, pSigs);
}

void APIENTRY
tritonCreateGeometryShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                           D3D10DDI_HSHADER h, D3D10DDI_HRTSHADER hRT, const VOID *pSigs)
{
    tritonCreateShaderCommon(hDevice, TRITON_SHADER_GS, pCode, h, hRT, pSigs);
}

void APIENTRY
tritonCreateHullShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                      D3D10DDI_HSHADER h, D3D10DDI_HRTSHADER hRT, const VOID *pSigs)
{
    tritonCreateShaderCommon(hDevice, TRITON_SHADER_HS, pCode, h, hRT, pSigs);
}

void APIENTRY
tritonCreateDomainShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                         D3D10DDI_HSHADER h, D3D10DDI_HRTSHADER hRT, const VOID *pSigs)
{
    tritonCreateShaderCommon(hDevice, TRITON_SHADER_DS, pCode, h, hRT, pSigs);
}

void APIENTRY
tritonCreateComputeShader(D3D10DDI_HDEVICE hDevice, const UINT *pCode,
                          D3D10DDI_HSHADER h, D3D10DDI_HRTSHADER hRT)
{
    tritonCreateShaderCommon(hDevice, TRITON_SHADER_CS, pCode, h, hRT, NULL);
}

void APIENTRY
tritonDestroyShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!s) return;
    if (pD && pD->pCurrentVS == s) pD->pCurrentVS = NULL;
    if (pD && pD->pCurrentGS == s) pD->pCurrentGS = NULL;
    if (pD && pD->pCurrentDS == s) pD->pCurrentDS = NULL;
    if (pD && pD->pCurrentPS == s) { pD->pCurrentPS = NULL; pD->pSentPS = NULL; }
    for (UINT i = 0; i < 4; ++i)
        if (s->apTirPS[i]) { ID3D11PixelShader_Release(s->apTirPS[i]); s->apTirPS[i] = NULL; }
    if (s->u.pDeviceChild) { ID3D11DeviceChild_Release(s->u.pDeviceChild); s->u.pDeviceChild = NULL; }
    if (s->pSoOnlyGS)      { ID3D11GeometryShader_Release(s->pSoOnlyGS);  s->pSoOnlyGS = NULL; }
    if (s->pSoDecl)        { HeapFree(GetProcessHeap(), 0, s->pSoDecl);    s->pSoDecl = NULL; }
    if (s->pBytecode)      { HeapFree(GetProcessHeap(), 0, s->pBytecode);  s->pBytecode = NULL; }
    s->cbBytecode = 0;
}

static void tritonBindSoOnlyGS(PTRITON_DEVICE pD);

void APIENTRY
tritonVsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD) return;
    pD->pBoundReconVS = NULL;
    pD->pCurrentVS = s;
    /* The resolver binds the VS itself (or its format-reconciled copy)
     * whenever it binds a layout. */
    if (!tritonResolveInputLayout(pD))
        ID3D11DeviceContext1_VSSetShader(pD->pCtx1,
            (s && s->u.pVS) ? s->u.pVS : NULL, NULL, 0);
    tritonBindSoOnlyGS(pD);
}

/* Target-independent rasterization: a ForcedSampleCount N > 1 with render
 * targets bound (single-sample, by the D3D11.1 rules) gives the pixel shader
 * an N-bit vCoverage, but the host rasterizes such a draw at one sample, so
 * under that state a PS reading vCoverage is replaced by a copy reading the
 * full N-bit mask (tritonDxbcFixInputCoverage).
 * Without render targets it is UAV-only rendering, which keeps the PS. */
static BOOL tritonTirActive(const PTRITON_DEVICE pD)
{
    return pD->rsForcedSampleCount > 1 && pD->omHasTargets &&
           pD->pCurrentPS && pD->pCurrentPS->fReadsCoverage;
}

/* The fixed-coverage copy of s for the bound ForcedSampleCount, made on the
 * first request for that count; NULL when it could not be made, or for a
 * count D3D11 does not allow. */
static ID3D11PixelShader *tritonTirPixelShader(PTRITON_DEVICE pD, PTRITON_SHADER s)
{
    const UINT n = pD->rsForcedSampleCount;
    if (n < 2 || n > 16 || (n & (n - 1)))
        return NULL;
    UINT slot = 0;
    while ((2u << slot) != n)
        ++slot;
    if (s->TirTried & (1u << slot))
        return s->apTirPS[slot];
    s->TirTried |= 1u << slot;
    SIZE_T cb = 0;
    void *p = tritonDxbcFixInputCoverage(s->pBytecode, s->cbBytecode,
                                         (1u << n) - 1u, &cb);
    if (!p) {
        TR_LOG("TIR: fixed-coverage copy of PS failed (ForcedSampleCount %u)", n);
        return NULL;
    }
    HRESULT hr = ID3D11Device1_CreatePixelShader(pD->pDev1, p, cb, NULL,
                                                 &s->apTirPS[slot]);
    HeapFree(GetProcessHeap(), 0, p);
    if (FAILED(hr)) {
        TR_LOG("TIR: CreatePixelShader for the fixed-coverage copy failed 0x%08lx", hr);
        s->apTirPS[slot] = NULL;
    }
    return s->apTirPS[slot];
}

/* The host PS the next draw needs: the tracked PS, or its fixed-coverage
 * copy under TIR. */
static ID3D11PixelShader *tritonWantedPixelShader(PTRITON_DEVICE pD)
{
    PTRITON_SHADER s = pD->pCurrentPS;
    if (!s)
        return NULL;
    if (tritonTirActive(pD)) {
        ID3D11PixelShader *tir = tritonTirPixelShader(pD, s);
        if (tir)
            return tir;
    }
    return s->u.pPS;
}

static void tritonSendPixelShader(PTRITON_DEVICE pD, ID3D11PixelShader *ps)
{
    pD->pSentPS = ps;
    ID3D11DeviceContext1_PSSetShader(pD->pCtx1, ps, NULL, 0);
}

/* After the rasterizer state or render targets change: rebind only when that
 * changes which form of the PS the draw needs. */
void tritonUpdateTirPixelShader(PTRITON_DEVICE pD)
{
    ID3D11PixelShader *ps = tritonWantedPixelShader(pD);
    if (ps != pD->pSentPS)
        tritonSendPixelShader(pD, ps);
}

void APIENTRY
tritonPsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD) return;
    pD->pCurrentPS = s;
    /* Always sent: the DXGI blit binds its own PS on the context and has the
     * runtime replay this DDI to restore the app's. */
    tritonSendPixelShader(pD, tritonWantedPixelShader(pD));
}

void APIENTRY
tritonGsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD) return;
    pD->pCurrentGS = s;
    if (s && s->fSoOnly) {
        tritonBindSoOnlyGS(pD);
        return;
    }
    ID3D11DeviceContext1_GSSetShader(pD->pCtx1,
        (s && s->u.pGS) ? s->u.pGS : NULL, NULL, 0);
}

void APIENTRY
tritonHsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD) return;
    ID3D11DeviceContext1_HSSetShader(pD->pCtx1,
        (s && s->u.pHS) ? s->u.pHS : NULL, NULL, 0);
}

void APIENTRY
tritonDsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD) return;
    pD->pCurrentDS = s;
    ID3D11DeviceContext1_DSSetShader(pD->pCtx1,
        (s && s->u.pDS) ? s->u.pDS : NULL, NULL, 0);
    tritonBindSoOnlyGS(pD);
}

void APIENTRY
tritonCsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD) return;
    ID3D11DeviceContext1_CSSetShader(pD->pCtx1,
        (s && s->u.pCS) ? s->u.pCS : NULL, NULL, 0);
}

/* Shaders that declare interfaces are bound through *SetShaderWithIfaces.
 * The host shaders are created without class linkage, so the class
 * instances have nowhere to go; bind the shader itself (the VS through
 * tritonVsSetShader so input-layout resolution still runs) rather than
 * leaving the previous shader bound. */
#define TR_SET_SHADER_WITH_IFACES(stage)                                       \
void APIENTRY                                                                  \
triton ## stage ## SetShaderWithIfaces(D3D10DDI_HDEVICE hDevice,               \
                                       D3D10DDI_HSHADER hShader,               \
                                       UINT NumClassInstances,                 \
                                       const UINT *pIfaces,                    \
                                       const D3D11DDIARG_POINTERDATA *pPtrs)   \
{                                                                              \
    (void)pIfaces; (void)pPtrs;                                                \
    if (NumClassInstances)                                                     \
        TR_STUB(#stage "SetShaderWithIfaces: class instances ignored");        \
    triton ## stage ## SetShader(hDevice, hShader);                            \
}
TR_SET_SHADER_WITH_IFACES(Vs)
TR_SET_SHADER_WITH_IFACES(Ps)
TR_SET_SHADER_WITH_IFACES(Gs)
TR_SET_SHADER_WITH_IFACES(Hs)
TR_SET_SHADER_WITH_IFACES(Ds)
TR_SET_SHADER_WITH_IFACES(Cs)
#undef TR_SET_SHADER_WITH_IFACES

/* ---------- Geometry Shader with Stream Output ----------
 *
 * D3D11's CreateGeometryShaderWithStreamOutput needs the per-element
 * D3D11_SO_DECLARATION_ENTRY filled with SemanticName + SemanticIndex,
 * but the DDI gives only Stream + RegisterIndex + RegisterMask +
 * OutputSlot. We resolve the missing fields by parsing the GS's DXBC
 * output signature (OSG5 for SM5, OSGN for SM4).
 *
 * The parser is bounds-checked and accepts only chunks we recognize. */

/* Packing can place several elements in one register (e.g. ATTRIB0 in .xy,
 * ATTRIB1 in .zw), so prefer the element whose mask covers the requested
 * component; fall back to the first element of the register. */
static const TritonDxbcSig *tritonDxbcFindSig(const TritonDxbcSig *p, UINT cnt,
                                              UINT stream, UINT reg, UINT comp)
{
    const TritonDxbcSig *fallback = NULL;
    for (UINT i = 0; i < cnt; ++i) {
        if (p[i].Stream != stream || p[i].Register != reg)
            continue;
        if (comp < 4 && (p[i].Mask & (1u << comp)))
            return &p[i];
        if (!fallback)
            fallback = &p[i];
    }
    return fallback;
}

/* Two flavors of the GS-with-SO create struct:
 *   10.0/10.1: D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT — no
 *     per-entry Stream, single StreamOutputStrideInBytes.
 *   11.0/11.1: D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT — per-entry
 *     Stream, BufferStridesInBytes[NumStrides], RasterizedStream. */

TR_CALC_PRIVATE_SIZE(tritonCalcPrivateGSWithSOSize, TRITON_SHADER,
                     const VOID *pArgs)

/* Fills the API SO declaration for a DDI one against the output signature
 * of the bytecode that will feed it. The DDI gives Stream + RegisterIndex +
 * RegisterMask + OutputSlot; SemanticName/Index come from the signature
 * (OSG5 for SM5, OSGN for SM4). `dxsigs` is caller storage the names point
 * into for as long as `dstEntries` is used.
 *
 * Returns FALSE when an entry has no signature element -- a declared gap, or
 * a register the signature lacks. Such an entry can only travel as a gap
 * (NULL name), which the host's stream-out cannot express: D3DMetal
 * dereferences the NULL name and the render-server worker dies, taking every
 * object of the process with it. The caller must not create from it. */
static BOOL tritonFillSoDecl(const void *pBytecode, SIZE_T cbBytecode,
                             const D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *pDdi,
                             UINT NumEntries,
                             D3D11_SO_DECLARATION_ENTRY *dstEntries,
                             TritonDxbcSig *dxsigs /* [128] */)
{
    const UINT cDxSigs = tritonDxbcParseSig(pBytecode, cbBytecode, FALSE,
                                             dxsigs, 128);
    if (cDxSigs == 0)
        TR_LOG("CreateGSWithSO: no output signature parsed");

    BOOL complete = TRUE;

    for (UINT i = 0; i < NumEntries; ++i) {
        const D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *src = &pDdi[i];

        /* Convert 4-bit RegisterMask -> contiguous (start, count). */
        BYTE m = src->RegisterMask & 0xF;
        BYTE start = 0;
        while (start < 4 && !(m & (1u << start))) ++start;
        BYTE count = 0;
        for (BYTE b = m; b; b >>= 1) count = (BYTE)(count + (b & 1));

        /* An SM5 GS output signature (OSG5/OSG1) carries per-entry streams;
         * OSGN entries all parse as stream 0, matching SM4's single stream. */
        const TritonDxbcSig *sig =
            tritonDxbcFindSig(dxsigs, cDxSigs, src->Stream, src->RegisterIndex,
                              start);

        /* The DDI RegisterMask is register-relative but the API's
         * StartComponent counts within the named element, so rebase against
         * the element's own first component (differs only for elements
         * packed away from .x). */
        BYTE elemBase = 0;
        if (sig) {
            while (elemBase < 4 && !(sig->Mask & (1u << elemBase)))
                ++elemBase;
            if (elemBase > start)
                elemBase = start;
        }

        if (!sig) {
            TR_LOG("CreateGSWithSO: SO entry %u (stream %u reg %u mask 0x%x) has "
                   "no output-signature match",
                   i, src->Stream, src->RegisterIndex, src->RegisterMask);
            complete = FALSE;
        }

        D3D11_SO_DECLARATION_ENTRY *dst = &dstEntries[i];
        dst->Stream         = src->Stream;
        dst->SemanticName   = sig ? sig->Name : NULL;   /* gap entry: NULL */
        dst->SemanticIndex  = sig ? sig->SemanticIndex : 0;
        dst->StartComponent = (BYTE)(start - (sig ? elemBase : 0));
        dst->ComponentCount = count;
        dst->OutputSlot     = (BYTE)src->OutputSlot;
    }
    return complete;
}

void APIENTRY
tritonCreateGSWithSO_11(D3D10DDI_HDEVICE hDevice,
                        const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pArgs,
                        D3D10DDI_HSHADER hShader,
                        D3D10DDI_HRTSHADER hRTShader,
                        const VOID *pSigsRaw)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD || !s) return;
    s->kind            = TRITON_SHADER_GS;
    s->pBytecode      = NULL;
    s->cbBytecode     = 0;
    s->u.pDeviceChild = NULL;
    s->cookie         = ++pD->nextShaderCookie;
    s->fSoOnly        = FALSE;
    s->pSoDecl        = NULL;
    s->cSoDecl        = 0;
    s->pSoOnlyGS      = NULL;
    s->fReadsCoverage = FALSE;
    memset(s->apTirPS, 0, sizeof(s->apTirPS));
    s->TirTried       = 0;
    if (pArgs->NumEntries > D3D11_SO_STREAM_COUNT * D3D11_SO_OUTPUT_COMPONENT_COUNT) {
        TR_LOG("CreateGSWithSO: NumEntries %u exceeds SO decl cap", pArgs->NumEntries);
        tritonSetCreateError(pD, E_INVALIDARG);
        return;
    }
    if (pArgs->NumStrides > D3D11_SO_BUFFER_SLOT_COUNT) {
        TR_LOG("CreateGSWithSO: NumStrides %u exceeds SO buffer slots", pArgs->NumStrides);
        tritonSetCreateError(pD, E_INVALIDARG);
        return;
    }
    /* No bytecode: a stream-output-only GS. The host makes one from the
     * source stage's bytecode, which is only known once that stage is bound,
     * so keep the declaration and build the object in tritonBindSoOnlyGS. */
    if (!pArgs->pShaderCode) {
        const SIZE_T cbDecl = pArgs->NumEntries * sizeof(*s->pSoDecl);
        if (cbDecl) {
            s->pSoDecl = (D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *)
                HeapAlloc(GetProcessHeap(), 0, cbDecl);
            if (!s->pSoDecl) {
                tritonSetCreateError(pD, E_OUTOFMEMORY);
                return;
            }
            memcpy(s->pSoDecl, pArgs->pOutputStreamDecl, cbDecl);
        }
        s->cSoDecl = pArgs->NumEntries;
        s->cSoStrides = pArgs->NumStrides;
        memcpy(s->SoStrides, pArgs->BufferStridesInBytes,
               pArgs->NumStrides * sizeof(UINT));
        s->SoRasterizedStream = pArgs->RasterizedStream;
        s->fSoOnly = TRUE;
        return;
    }

    const SIZE_T cbTokens = tritonShaderTokenBytes(pArgs->pShaderCode);
    TritonStageSigs sigs;
    tritonUnpackSigs(pSigsRaw, FALSE, &sigs);

    if (sigs.cIn > 4096 || sigs.cOut > 4096) {
        TR_LOG("CreateGSWithSO: signature counts exceed 4096 (in=%u out=%u)",
               sigs.cIn, sigs.cOut);
        tritonSetCreateError(pD, E_INVALIDARG);
        return;
    }

    SIZE_T cbDxbc = 0;
    void *pDxbc = tritonBuildDxbc(pArgs->pShaderCode, cbTokens,
                                   sigs.pIn,  sigs.cIn,
                                   sigs.pOut, sigs.cOut,
                                   NULL, 0,
                                   tritonSigStride(pD),
                                   &cbDxbc);
    if (!pDxbc) {
        TR_LOG("CreateGSWithSO: tritonBuildDxbc failed");
        tritonSetCreateError(pD, E_OUTOFMEMORY);
        return;
    }
    s->pBytecode  = pDxbc;
    s->cbBytecode = cbDxbc;

    D3D11_SO_DECLARATION_ENTRY soEntries[D3D11_SO_STREAM_COUNT *
                                         D3D11_SO_OUTPUT_COMPONENT_COUNT];
    TritonDxbcSig dxsigs[128];
    if (!tritonFillSoDecl(s->pBytecode, s->cbBytecode, pArgs->pOutputStreamDecl,
                          pArgs->NumEntries, soEntries, dxsigs)) {
        /* No pfnDestroyShader after a create error - free the container. */
        HeapFree(GetProcessHeap(), 0, s->pBytecode);
        s->pBytecode  = NULL;
        s->cbBytecode = 0;
        tritonSetCreateError(pD, E_INVALIDARG);
        return;
    }

    HRESULT hr = ID3D11Device1_CreateGeometryShaderWithStreamOutput(
        pD->pDev1,
        s->pBytecode, s->cbBytecode,
        soEntries, pArgs->NumEntries,
        pArgs->BufferStridesInBytes, pArgs->NumStrides,
        pArgs->RasterizedStream,
        NULL,
        &s->u.pGS);
    if (FAILED(hr)) {
        TR_LOG("CreateGSWithSO: D3D11 create failed 0x%08lx", hr);
        s->u.pGS = NULL;
        /* No pfnDestroyShader after a create error - free the container. */
        HeapFree(GetProcessHeap(), 0, s->pBytecode);
        s->pBytecode  = NULL;
        s->cbBytecode = 0;
        tritonSetCreateError(pD, hr);
        return;
    }
    tritonShaderDropUnusedBytecode(s);
}

/* Binds the host object of a stream-output-only GS, made from the bound
 * source stage's bytecode (DS when tessellating, else VS) and remade when
 * that stage changed since the last bind. Nothing binds until a source
 * exists. */
static void tritonBindSoOnlyGS(PTRITON_DEVICE pD)
{
    PTRITON_SHADER gs = pD->pCurrentGS;
    if (!gs || !gs->fSoOnly)
        return;
    PTRITON_SHADER src = pD->pCurrentDS ? pD->pCurrentDS : pD->pCurrentVS;
    if (!src || !src->pBytecode) {
        ID3D11DeviceContext1_GSSetShader(pD->pCtx1, NULL, NULL, 0);
        return;
    }
    if (!gs->pSoOnlyGS || gs->SoSourceCookie != src->cookie) {
        if (gs->pSoOnlyGS) {
            ID3D11GeometryShader_Release(gs->pSoOnlyGS);
            gs->pSoOnlyGS = NULL;
        }
        D3D11_SO_DECLARATION_ENTRY soEntries[D3D11_SO_STREAM_COUNT *
                                             D3D11_SO_OUTPUT_COMPONENT_COUNT];
        TritonDxbcSig dxsigs[128];
        HRESULT hr = E_INVALIDARG;
        if (tritonFillSoDecl(src->pBytecode, src->cbBytecode, gs->pSoDecl,
                             gs->cSoDecl, soEntries, dxsigs))
            hr = ID3D11Device1_CreateGeometryShaderWithStreamOutput(
                pD->pDev1, src->pBytecode, src->cbBytecode,
                soEntries, gs->cSoDecl, gs->SoStrides, gs->cSoStrides,
                gs->SoRasterizedStream, NULL, &gs->pSoOnlyGS);
        if (FAILED(hr)) {
            TR_LOG("SO-only GS: D3D11 create from %s bytecode failed 0x%08lx",
                   pD->pCurrentDS ? "DS" : "VS", hr);
            gs->pSoOnlyGS = NULL;
        }
        gs->SoSourceCookie = src->cookie;
    }
    ID3D11DeviceContext1_GSSetShader(pD->pCtx1, gs->pSoOnlyGS, NULL, 0);
}

/* ---------- Lazy CreateInputLayout ----------
 *
 * The DDI's input element descriptors carry InputRegister (the shader
 * register index), not a SemanticName. D3D11 CreateInputLayout demands
 * SemanticName. We bridge by parsing the current VS's DXBC input
 * signature (ISGN/ISG1) at bind time.
 *
 * Cached per (ElementLayout, VS instance) in a small LRU set
 * (TRITON_IL_CACHE_SIZE), so switching between the vertex shaders that
 * share a layout rebinds instead of rebuilding. The key is the VS's
 * monotonic cookie, not its bytecode heap pointer — a freed VS pointer
 * reused at the same address would otherwise alias a layout built against
 * the old VS's input signature. */

/* Drop a reconciled VS left over from a previous layout.  Costs one pointer
 * test when none was ever bound, which is the common case. */
void tritonUnbindReconVs(PTRITON_DEVICE pD)
{
    if (!pD || !pD->pBoundReconVS)
        return;
    pD->pBoundReconVS = NULL;
    ID3D11DeviceContext1_VSSetShader(pD->pCtx1,
        (pD->pCurrentVS && pD->pCurrentVS->u.pVS) ? pD->pCurrentVS->u.pVS : NULL,
        NULL, 0);
}

/* Returns TRUE when it bound a layout and, with it, the VS. */
BOOL tritonResolveInputLayout(PTRITON_DEVICE pD)
{
    if (!pD) return FALSE;
    PTRITON_ELEMENTLAYOUT e  = pD->pCurrentLayout;
    PTRITON_SHADER        vs = pD->pCurrentVS;
    if (!e || !vs || !vs->pBytecode || !e->pElements || e->NumElements == 0) {
        tritonUnbindReconVs(pD);
        return FALSE;
    }
    /* Hit: a host layout already built for this VS. */
    TRITON_IL_ENTRY *ent = NULL;
    for (UINT i = 0; i < TRITON_IL_CACHE_SIZE; ++i) {
        if (e->aIl[i].pLayout && e->aIl[i].VsCookie == vs->cookie) {
            ent = &e->aIl[i];
            break;
        }
    }
    if (ent) {
        ent->LastUse = ++e->IlClock;
        ID3D11DeviceContext1_IASetInputLayout(pD->pCtx1, ent->pLayout);
        /* Bind the format-reconciled VS if this layout needed one; otherwise
         * re-assert the app's VS, so a reconciled VS left bound for a previous
         * layout does not leak into this draw. */
        ID3D11DeviceContext1_VSSetShader(pD->pCtx1,
            ent->pReconVS ? ent->pReconVS : vs->u.pVS, NULL, 0);
        pD->pBoundReconVS = ent->pReconVS;
        return TRUE;
    }
    /* Miss: build into an empty slot, else evict the least recently used.
     * Releasing a layout or reconciled VS that is still bound is safe: the
     * context keeps its own reference until the binding below replaces it. */
    ent = &e->aIl[0];
    for (UINT i = 0; i < TRITON_IL_CACHE_SIZE; ++i) {
        if (!e->aIl[i].pLayout) { ent = &e->aIl[i]; break; }
        if (e->aIl[i].LastUse < ent->LastUse)
            ent = &e->aIl[i];
    }
    if (ent->pLayout) {
        ID3D11InputLayout_Release(ent->pLayout);
        ent->pLayout = NULL;
    }
    if (ent->pReconVS) {
        ID3D11VertexShader_Release(ent->pReconVS);
        ent->pReconVS = NULL;
    }
    ent->VsCookie = 0;

    TritonDxbcSig sigs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT * 2];
    const UINT cSigs = tritonDxbcParseSig(vs->pBytecode, vs->cbBytecode,
                                          TRUE, sigs,
                                          sizeof(sigs) / sizeof(sigs[0]));
    if (cSigs == 0) {
        TR_LOG("ResolveInputLayout: VS has no ISGN/ISG1 chunk");
        tritonUnbindReconVs(pD);
        return FALSE;
    }

    D3D11_INPUT_ELEMENT_DESC descs[D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENTS_COMPONENTS];
    if (e->NumElements > (sizeof(descs) / sizeof(descs[0]))) {
        TR_LOG("ResolveInputLayout: %u elements exceeds cap", e->NumElements);
        tritonUnbindReconVs(pD);
        return FALSE;
    }

    /* Collect the matched entries to synthesise a signature describing
     * exactly this element set (see below). */
    const char *sigNames[D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENTS_COMPONENTS];
    UINT        sigSemIdx[D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENTS_COMPONENTS];
    UINT        sigRegs[D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENTS_COMPONENTS];
    UINT        cMatched = 0;

    for (UINT i = 0; i < e->NumElements; ++i) {
        const D3D10DDIARG_INPUT_ELEMENT_DESC *src = &e->pElements[i];
        D3D11_INPUT_ELEMENT_DESC             *dst = &descs[i];

        const TritonDxbcSig *sig = NULL;
        for (UINT j = 0; j < cSigs; ++j) {
            if (sigs[j].Register == src->InputRegister) {
                sig = &sigs[j];
                break;
            }
        }
        dst->SemanticName         = sig ? sig->Name : "";
        dst->SemanticIndex        = sig ? sig->SemanticIndex : 0;
        dst->Format               = src->Format;
        dst->InputSlot            = src->InputSlot;
        dst->AlignedByteOffset    = src->AlignedByteOffset;
        dst->InputSlotClass       = (D3D11_INPUT_CLASSIFICATION)src->InputSlotClass;
        dst->InstanceDataStepRate = src->InstanceDataStepRate;

        if (sig) {
            sigNames[cMatched]  = sig->Name;
            sigSemIdx[cMatched] = sig->SemanticIndex;
            sigRegs[cMatched]   = src->InputRegister;
            ++cMatched;
        }
    }

    /* The vertex formats are only known here, so derive the component type per
     * input register from them and, when the VS ISGN disagrees, bind a
     * corrected private copy of the VS in its place (rationale in
     * tritonReconcileVsInputSig). */
    unsigned char compReg[32] = { 0 };
    for (UINT i = 0; i < e->NumElements; ++i) {
        UINT reg = e->pElements[i].InputRegister;
        if (reg >= 32) continue;
        compReg[reg] = tritonDxbcFormatCompType((UINT)e->pElements[i].Format);
    }
    SIZE_T cbRecon = 0;
    void *pRecon = NULL;
    if (npt_host_workaround_flags() & NPT_WA_TYPE_VS_INPUT_FROM_VERTEX_FORMAT)
        pRecon = tritonReconcileVsInputSig(vs->pBytecode, vs->cbBytecode,
                                           compReg, &cbRecon);
    if (pRecon) {
        HRESULT hrv = ID3D11Device1_CreateVertexShader(pD->pDev1, pRecon, cbRecon,
                                                       NULL, &ent->pReconVS);
        if (FAILED(hrv)) {
            TR_LOG("ResolveInputLayout: reconciled CreateVertexShader failed 0x%08lx", hrv);
            ent->pReconVS = NULL;
        }
        HeapFree(GetProcessHeap(), 0, pRecon);
    }

    /* CreateInputLayout validates the element descs against an input
     * signature, and the bound VS frequently declares more inputs than this
     * layout binds, which dxvk rejects (see tritonBuildInputSigDxbc).  Pass a
     * synthesised signature mirroring exactly the matched elements; the layout
     * is still re-validated against the real VS at draw-time pipeline
     * creation. */
    const void *pSigBc  = vs->pBytecode;
    SIZE_T      cbSigBc  = vs->cbBytecode;
    void       *pSynthBc = NULL;
    if (cMatched > 0) {
        SIZE_T cbSynth = 0;
        pSynthBc = tritonBuildInputSigDxbc(sigNames, sigSemIdx, sigRegs,
                                           cMatched, &cbSynth);
        if (pSynthBc) { pSigBc = pSynthBc; cbSigBc = cbSynth; }
    }

    HRESULT hr = ID3D11Device1_CreateInputLayout(pD->pDev1, descs, e->NumElements,
                                                 pSigBc, cbSigBc, &ent->pLayout);
    if (pSynthBc)
        HeapFree(GetProcessHeap(), 0, pSynthBc);
    if (FAILED(hr)) {
        TR_LOG("ResolveInputLayout: CreateInputLayout failed 0x%08lx", hr);
        ent->pLayout = NULL;
        ent->VsCookie = 0;
        if (ent->pReconVS) {
            ID3D11VertexShader_Release(ent->pReconVS);
            ent->pReconVS = NULL;
        }
        tritonUnbindReconVs(pD);
        return FALSE;
    }
    ent->VsCookie = vs->cookie;
    ent->LastUse  = ++e->IlClock;
    ID3D11DeviceContext1_IASetInputLayout(pD->pCtx1, ent->pLayout);
    ID3D11DeviceContext1_VSSetShader(pD->pCtx1,
        ent->pReconVS ? ent->pReconVS : vs->u.pVS, NULL, 0);
    pD->pBoundReconVS = ent->pReconVS;
    return TRUE;
}

/* WDDM 2.0: RetrieveShaderComment. Triton's shader path is pure DXBC
 * passthrough; there is no driver-side commentary to surface. Return an
 * empty string with count=1 (just the NUL). */
HRESULT APIENTRY
tritonRetrieveShaderComment(D3D10DDI_HDEVICE hDev, D3D10DDI_HSHADER hShader,
                            WCHAR *pBuffer, SIZE_T *pCharacterCount)
{
    TR_STUB("RetrieveShaderComment (DXBC passthrough; no driver commentary)");
    if (pCharacterCount && *pCharacterCount > 0 && pBuffer)
        pBuffer[0] = L'\0';
    if (pCharacterCount) *pCharacterCount = 1;
    return S_OK;
}
