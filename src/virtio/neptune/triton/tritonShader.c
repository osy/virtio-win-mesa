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

/* Declared early so tritonVsSetShader can call it. */
void tritonResolveInputLayout(PTRITON_DEVICE pD);

/* DXBC wrapper from tritonDxbc.c. */
void *tritonBuildDxbc(const UINT *pTokens, SIZE_T cbTokens,
                                  const void *pInEntries,    UINT cInSigs,
                                  const void *pOutEntries,   UINT cOutSigs,
                                  const void *pPatchEntries, UINT cPatchSigs,
                                  UINT entryStride,
                                  SIZE_T *pcbOut);

/* Synthetic ISGN-only container builder from tritonDxbc.c. */
void *tritonBuildInputSigDxbc(const char *const *pNames,
                              const UINT *pSemanticIndices,
                              const UINT *pRegisters,
                              UINT cEntries, SIZE_T *pcbOut);

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

SIZE_T APIENTRY
tritonCalcPrivateShaderSize(D3D10DDI_HDEVICE hDev, const UINT *pTokens, const VOID *pArgs)
{
    return sizeof(TRITON_SHADER);
}

/* Tessellation shader size — same as regular shader size. The DDI hands
 * tessellation IO signatures separately but we forward DXBC unchanged.
 * Must return non-zero: a 0-sized allocation would cause the create
 * handler to write into a 0-byte buffer. */
SIZE_T APIENTRY
tritonCalcPrivateTessellationShaderSize(D3D10DDI_HDEVICE hDev, const UINT *pTokens, const VOID *pArgs)
{
    return sizeof(TRITON_SHADER);
}

/* Unpack a stage-IO or tessellation-IO signatures struct from a raw
 * void*. D3D10DDIARG_STAGE_IO_SIGNATURES (32 B) and
 * D3D11_1DDIARG_STAGE_IO_SIGNATURES (32 B) share the same head layout —
 * 2 (ptr, count) pairs with natural x64 alignment. The tessellation
 * variant tacks on a third pair for patch-constant signatures. */
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
    out->pIn  = *(const void *const *)(p +  0);
    out->cIn  = *(const UINT *)(p +  8);
    out->pOut = *(const void *const *)(p + 16);
    out->cOut = *(const UINT *)(p + 24);
    if (fTess) {
        out->pPatch = *(const void *const *)(p + 32);
        out->cPatch = *(const UINT *)(p + 40);
    }
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
    s->hRT            = hRTShader;
    s->kind           = kind;
    s->pBytecode      = NULL;
    s->cbBytecode     = 0;
    s->u.pDeviceChild = NULL;
    s->cookie         = ++pD->nextShaderCookie;

    const SIZE_T cbTokens = tritonShaderTokenBytes(pShaderCode);
    /* Compute shaders have no input/output signature; HS/DS carry an
     * additional patch-constant signature. */
    const BOOL fTess = (kind == TRITON_SHADER_HS || kind == TRITON_SHADER_DS);
    TritonStageSigs sigs;
    tritonUnpackSigs(pSigsRaw, fTess, &sigs);

    /* Each signature array capped at 4096 entries. */
    if (sigs.cIn > 4096 || sigs.cOut > 4096 || sigs.cPatch > 4096) {
        TR_LOG("CreateShader[%d]: signature counts exceed 4096 (in=%u out=%u patch=%u)",
               (int)kind, sigs.cIn, sigs.cOut, sigs.cPatch);
        tritonSetError(pD, E_INVALIDARG);
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
        tritonSetError(pD, E_OUTOFMEMORY);
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
        tritonSetError(pD, hr);
    }
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
    if (s->u.pDeviceChild) { ID3D11DeviceChild_Release(s->u.pDeviceChild); s->u.pDeviceChild = NULL; }
    if (s->pBytecode)      { HeapFree(GetProcessHeap(), 0, s->pBytecode);  s->pBytecode = NULL; }
    s->cbBytecode = 0;
}

void APIENTRY
tritonVsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD) return;
    ID3D11VertexShader *vs = (s && s->u.pVS) ? s->u.pVS : NULL;
    ID3D11DeviceContext1_VSSetShader(pD->pCtx1, vs, NULL, 0);
    pD->pCurrentVS = s;
    tritonResolveInputLayout(pD);
}

void APIENTRY
tritonPsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD) return;
    ID3D11PixelShader *ps = (s && s->u.pPS) ? s->u.pPS : NULL;
    ID3D11DeviceContext1_PSSetShader(pD->pCtx1, ps, NULL, 0);
}

void APIENTRY
tritonGsSetShader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_SHADER s  = (PTRITON_SHADER)(hShader.pDrvPrivate);
    if (!pD) return;
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
    ID3D11DeviceContext1_DSSetShader(pD->pCtx1,
        (s && s->u.pDS) ? s->u.pDS : NULL, NULL, 0);
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

/* ---------- Geometry Shader with Stream Output ----------
 *
 * D3D11's CreateGeometryShaderWithStreamOutput needs the per-element
 * D3D11_SO_DECLARATION_ENTRY filled with SemanticName + SemanticIndex,
 * but the DDI gives only Stream + RegisterIndex + RegisterMask +
 * OutputSlot. We resolve the missing fields by parsing the GS's DXBC
 * output signature (OSG5 for SM5, OSGN for SM4).
 *
 * The parser is bounds-checked and accepts only chunks we recognize. */

#define TRITON_DXBC_MAGIC 0x43425844u  /* "DXBC" */
#define TRITON_TAG_OSGN   0x4E47534Fu  /* "OSGN" — output signature, no Stream */
#define TRITON_TAG_OSG5   0x3547534Fu  /* "OSG5" — adds Stream */
#define TRITON_TAG_OSG1   0x3147534Fu  /* "OSG1" — Stream + MinPrecision */
#define TRITON_TAG_ISGN   0x4E475349u  /* "ISGN" — input signature */
#define TRITON_TAG_ISG1   0x31475349u  /* "ISG1" — input + MinPrecision */

typedef struct TritonDxbcSig {
    UINT Stream;
    UINT Register;
    UINT SemanticIndex;
    char Name[64];
} TritonDxbcSig;

/* Generic OSG-style signature parser. fInput=TRUE searches for the input
 * signature (ISGN/ISG1); FALSE for output (OSG5/OSG1/OSGN). */
static UINT tritonDxbcParseSig(const void *pBytecode, SIZE_T cbBytecode,
                               BOOL fInput,
                               TritonDxbcSig *pOut, UINT maxEntries)
{
    if (!pBytecode || cbBytecode < 32 || !pOut || !maxEntries) return 0;
    const BYTE  *p   = (const BYTE *)(pBytecode);
    const UINT  *pH  = (const UINT *)(p);
    if (pH[0] != TRITON_DXBC_MAGIC) return 0;

    UINT fileSize = pH[6];
    UINT cChunks  = pH[7];
    if (fileSize > cbBytecode) fileSize = (UINT)cbBytecode;
    if (cChunks == 0 || cChunks > 256) return 0;
    if (32 + cChunks * 4u > fileSize) return 0;

    const UINT *pOffsets = (const UINT *)(p + 32);

    /* Tags by priority — newer variants (with extra fields) first.
     *   OSGN/ISGN: 24 B entries (no Stream)
     *   OSG5:      28 B entries (Stream prefix)
     *   OSG1/ISG1: 32 B entries (Stream prefix + MinPrecision suffix) */
    const UINT tagPrefer = fInput ? TRITON_TAG_ISG1 : TRITON_TAG_OSG1;
    const UINT tagMid    = fInput ? TRITON_TAG_ISG1 : TRITON_TAG_OSG5;
    const UINT tagBasic  = fInput ? TRITON_TAG_ISGN : TRITON_TAG_OSGN;
    UINT  dataOff = 0, dataLen = 0;
    UINT  entrySize = 0;
    BOOL  fHasStream = FALSE;
    for (UINT i = 0; i < cChunks; ++i) {
        UINT off = pOffsets[i];
        if (off > fileSize - 8) continue;
        UINT tag = *(const UINT *)(p + off);
        UINT len = *(const UINT *)(p + off + 4);
        if (len > fileSize - off - 8) continue;

        if (tag == tagPrefer) {
            dataOff    = off + 8;
            dataLen    = len;
            entrySize  = 32u;
            fHasStream = !fInput;  /* ISG1 has no Stream */
            break;
        }
        if (tag == tagMid && !fInput) {
            dataOff    = off + 8;
            dataLen    = len;
            entrySize  = 28u;
            fHasStream = TRUE;
            break;
        }
        if (tag == tagBasic && dataOff == 0) {
            dataOff    = off + 8;
            dataLen    = len;
            entrySize  = 24u;
            fHasStream = FALSE;
        }
    }
    if (dataOff == 0 || dataLen < 8 || entrySize == 0) return 0;

    const BYTE *pData = p + dataOff;
    UINT cElem = *(const UINT *)(pData + 0);
    if (cElem == 0 || cElem > 256) return 0;
    if (8u + cElem * entrySize > dataLen) return 0;

    UINT cWritten = 0;
    for (UINT i = 0; i < cElem && cWritten < maxEntries; ++i) {
        const BYTE *pe = pData + 8 + i * entrySize;
        /* Field offsets (OSG5/OSG1 share these; OSGN omits Stream):
         *      Stream  NameOff  SemIdx  Register  Mask
         *  OSGN  -        0       4       16       20
         *  OSG5  0        4       8       20       24
         *  OSG1  0        4       8       20       24   (+ MinPrecision @ 28) */
        UINT stream = 0, off, semIdx, reg;
        if (fHasStream) {
            stream = *(const UINT *)(pe + 0);
            off    = *(const UINT *)(pe + 4);
            semIdx = *(const UINT *)(pe + 8);
            reg    = *(const UINT *)(pe + 20);
        } else {
            off    = *(const UINT *)(pe + 0);
            semIdx = *(const UINT *)(pe + 4);
            reg    = *(const UINT *)(pe + 16);
        }
        if (off >= dataLen) continue;

        TritonDxbcSig *out = &pOut[cWritten];
        out->Stream        = stream;
        out->Register      = reg;
        out->SemanticIndex = semIdx;

        const char *psz   = (const char *)(pData + off);
        UINT        avail = dataLen - off;
        UINT        j     = 0;
        const UINT  cap   = sizeof(out->Name) - 1;
        while (j < cap && j < avail && psz[j]) {
            out->Name[j] = psz[j];
            ++j;
        }
        out->Name[j] = '\0';
        ++cWritten;
    }
    return cWritten;
}

static const TritonDxbcSig *tritonDxbcFindSig(const TritonDxbcSig *p, UINT cnt,
                                              UINT stream, UINT reg)
{
    for (UINT i = 0; i < cnt; ++i)
        if (p[i].Stream == stream && p[i].Register == reg)
            return &p[i];
    return NULL;
}

/* Two flavors of the GS-with-SO create struct:
 *   10.0/10.1: D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT — no
 *     per-entry Stream, single StreamOutputStrideInBytes.
 *   11.0/11.1: D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT — per-entry
 *     Stream, BufferStridesInBytes[NumStrides], RasterizedStream. */

SIZE_T APIENTRY
tritonCalcPrivateGSWithSOSize(D3D10DDI_HDEVICE hDev, const VOID *pArgs)
{
    return sizeof(TRITON_SHADER);
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
    s->hRT            = hRTShader;
    s->kind            = TRITON_SHADER_GS;
    s->pBytecode      = NULL;
    s->cbBytecode     = 0;
    s->u.pDeviceChild = NULL;
    s->cookie         = ++pD->nextShaderCookie;
    /* ID3D11Device1::CreateGeometryShaderWithStreamOutput mandates
     * non-NULL bytecode; SO-alias shaders (no bytecode, decls only) have
     * no equivalent. */
    if (!pArgs->pShaderCode) {
        TR_LOG("CreateGSWithSO: NULL pShaderCode (SO-alias shader unsupported)");
        tritonSetError(pD, E_NOTIMPL);
        return;
    }
    if (pArgs->NumEntries > D3D11_SO_STREAM_COUNT * D3D11_SO_OUTPUT_COMPONENT_COUNT) {
        TR_LOG("CreateGSWithSO: NumEntries %u exceeds SO decl cap", pArgs->NumEntries);
        tritonSetError(pD, E_INVALIDARG);
        return;
    }

    const SIZE_T cbTokens = tritonShaderTokenBytes(pArgs->pShaderCode);
    TritonStageSigs sigs;
    tritonUnpackSigs(pSigsRaw, FALSE, &sigs);

    if (sigs.cIn > 4096 || sigs.cOut > 4096) {
        TR_LOG("CreateGSWithSO: signature counts exceed 4096 (in=%u out=%u)",
               sigs.cIn, sigs.cOut);
        tritonSetError(pD, E_INVALIDARG);
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
        tritonSetError(pD, E_OUTOFMEMORY);
        return;
    }
    s->pBytecode  = pDxbc;
    s->cbBytecode = cbDxbc;

    /* The SO declaration's SemanticName/Index must match the GS's OSGN.
     * Since we synthesise the OSGN from the DDI signature entries, the
     * parser will return matching names. */
    TritonDxbcSig dxsigs[64];
    const UINT cDxSigs = tritonDxbcParseSig(s->pBytecode, s->cbBytecode,
                                             FALSE, dxsigs, 64);

    D3D11_SO_DECLARATION_ENTRY soStack[D3D11_SO_STREAM_COUNT *
                                       D3D11_SO_OUTPUT_COMPONENT_COUNT];
    D3D11_SO_DECLARATION_ENTRY *soEntries = soStack;
    if (pArgs->NumEntries > (sizeof(soStack) / sizeof(soStack[0]))) {
        soEntries = (D3D11_SO_DECLARATION_ENTRY *)(
            HeapAlloc(GetProcessHeap(), 0,
                      pArgs->NumEntries * sizeof(D3D11_SO_DECLARATION_ENTRY)));
        if (!soEntries) {
            TR_LOG("CreateGSWithSO: HeapAlloc for SO entries failed");
            HeapFree(GetProcessHeap(), 0, s->pBytecode);
            s->pBytecode = NULL;
            s->cbBytecode = 0;
            tritonSetError(pD, E_OUTOFMEMORY);
            return;
        }
    }

    for (UINT i = 0; i < pArgs->NumEntries; ++i) {
        const D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *src =
            &pArgs->pOutputStreamDecl[i];
        /* Match by register only — the wrapper writes every entry at
         * stream 0 (multi-stream encoding would need OSG5). */
        const TritonDxbcSig *sig =
            tritonDxbcFindSig(dxsigs, cDxSigs, 0, src->RegisterIndex);

        /* Convert 4-bit RegisterMask -> contiguous (start, count). */
        BYTE m = src->RegisterMask & 0xF;
        BYTE start = 0;
        while (start < 4 && !(m & (1u << start))) ++start;
        BYTE count = 0;
        for (BYTE b = m; b; b >>= 1) count = (BYTE)(count + (b & 1));

        D3D11_SO_DECLARATION_ENTRY *dst = &soEntries[i];
        dst->Stream         = src->Stream;
        dst->SemanticName   = sig ? sig->Name : NULL;   /* gap entry: NULL */
        dst->SemanticIndex  = sig ? sig->SemanticIndex : 0;
        dst->StartComponent = start;
        dst->ComponentCount = count;
        dst->OutputSlot     = (BYTE)src->OutputSlot;
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
        /* No pfnDestroyShader after a create error — free the container. */
        HeapFree(GetProcessHeap(), 0, s->pBytecode);
        s->pBytecode  = NULL;
        s->cbBytecode = 0;
        tritonSetError(pD, hr);
    }

    if (soEntries != soStack)
        HeapFree(GetProcessHeap(), 0, soEntries);
}

/* ---------- Lazy CreateInputLayout ----------
 *
 * The DDI's input element descriptors carry InputRegister (the shader
 * register index), not a SemanticName. D3D11 CreateInputLayout demands
 * SemanticName. We bridge by parsing the current VS's DXBC input
 * signature (ISGN/ISG1) at bind time.
 *
 * Cached per (ElementLayout, VS instance): recreate only when the bound
 * VS changes. The key is the VS's monotonic cookie, not its bytecode
 * heap pointer — a freed VS pointer reused at the same address would
 * otherwise alias a layout built against the old VS's input signature. */

void tritonResolveInputLayout(PTRITON_DEVICE pD)
{
    if (!pD) return;
    PTRITON_ELEMENTLAYOUT e  = pD->pCurrentLayout;
    PTRITON_SHADER        vs = pD->pCurrentVS;
    if (!e || !vs || !vs->pBytecode || !e->pElements || e->NumElements == 0)
        return;
    if (e->pLayout && e->LayoutVsCookie == vs->cookie) {
        ID3D11DeviceContext1_IASetInputLayout(pD->pCtx1, e->pLayout);
        return;
    }
    if (e->pLayout) {
        ID3D11InputLayout_Release(e->pLayout);
        e->pLayout = NULL;
        e->LayoutVsCookie = 0;
    }

    TritonDxbcSig sigs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT * 2];
    const UINT cSigs = tritonDxbcParseSig(vs->pBytecode, vs->cbBytecode,
                                          TRUE, sigs,
                                          sizeof(sigs) / sizeof(sigs[0]));
    if (cSigs == 0) {
        TR_LOG("ResolveInputLayout: VS has no ISGN/ISG1 chunk");
        return;
    }

    D3D11_INPUT_ELEMENT_DESC descs[D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENTS_COMPONENTS];
    if (e->NumElements > (sizeof(descs) / sizeof(descs[0]))) {
        TR_LOG("ResolveInputLayout: %u elements exceeds cap", e->NumElements);
        return;
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

    /* CreateInputLayout validates the element descs against an input
     * signature.  D3D11 permits a layout that covers only a subset of the
     * bound VS's inputs (the rest read as zero), but dxvk's CreateInputLayout
     * rejects the layout if any signature input lacks a matching element.
     * The bound VS frequently declares more inputs than this layout binds,
     * so handing dxvk the full VS bytecode fails.  Instead synthesise a
     * signature that mirrors exactly the matched elements; the layout is
     * still re-validated against the real VS at draw-time pipeline creation. */
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
                                                 pSigBc, cbSigBc, &e->pLayout);
    if (pSynthBc)
        HeapFree(GetProcessHeap(), 0, pSynthBc);
    if (FAILED(hr)) {
        TR_LOG("ResolveInputLayout: CreateInputLayout failed 0x%08lx", hr);
        e->pLayout = NULL;
        e->LayoutVsCookie = 0;
        return;
    }
    e->LayoutVsCookie = vs->cookie;
    ID3D11DeviceContext1_IASetInputLayout(pD->pCtx1, e->pLayout);
}

/* The 10.x variant — no per-entry Stream, just one stride. Upgrade to
 * 11.x in-place and reuse the 11.x path. */
void APIENTRY
tritonCreateGSWithSO_10(D3D10DDI_HDEVICE hDevice,
                        const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *pArgs,
                        D3D10DDI_HSHADER hShader,
                        D3D10DDI_HRTSHADER hRTShader,
                        const VOID *pSigs)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT up11 = {};
    D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY upDecl[D3D11_SO_STREAM_COUNT *
                                                       D3D11_SO_OUTPUT_COMPONENT_COUNT] = {};
    D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *pUp = upDecl;
    if (pArgs->NumEntries > (sizeof(upDecl) / sizeof(upDecl[0]))) {
        pUp = (D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY *)(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                      pArgs->NumEntries * sizeof(*pUp)));
        if (!pUp) {
            TR_LOG("CreateGSWithSO_10: HeapAlloc failed");
            if (pD) tritonSetError(pD, E_OUTOFMEMORY);
            return;
        }
    }
    for (UINT i = 0; i < pArgs->NumEntries; ++i) {
        pUp[i].Stream        = 0;
        pUp[i].OutputSlot    = pArgs->pOutputStreamDecl[i].OutputSlot;
        pUp[i].RegisterIndex = pArgs->pOutputStreamDecl[i].RegisterIndex;
        pUp[i].RegisterMask  = pArgs->pOutputStreamDecl[i].RegisterMask;
    }
    UINT stride = pArgs->StreamOutputStrideInBytes;
    up11.pShaderCode          = pArgs->pShaderCode;
    up11.pOutputStreamDecl    = pUp;
    up11.NumEntries           = pArgs->NumEntries;
    up11.BufferStridesInBytes = &stride;
    up11.NumStrides           = 1;
    up11.RasterizedStream     = 0;

    tritonCreateGSWithSO_11(hDevice, &up11, hShader, hRTShader, pSigs);

    if (pUp != upDecl) HeapFree(GetProcessHeap(), 0, pUp);
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
