/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 DDI pipeline objects: shaders, fixed-function state objects,
 * root signatures, PSOs:
 *  - shader creates rebuild a full DXBC container from the DDI's driver
 *    bytecode + IO signatures via tritonBuildDxbc (the D3D11 synthetic-
 *    container machinery) and store it;
 *  - state creates store the DDI desc verbatim;
 *  - root-signature create converts the deserialized DDI 1.1 struct to
 *    the API 1.1 desc and reuses the wire serialize path
 *    (npt_D3D12SerializeVersionedRootSignature) before inner
 *    CreateRootSignature;
 *  - PSO create assembles the API graphics/compute desc from the stored
 *    pieces and forwards to the inner device.
 */

#include "triton12.h"
#include "triton_log.h"

#include "tritonDxbc.h"
#include "tritonDxil.h"
#include "npt_workaround.h"

/* npt_entry_d3d12.c (aliased to the public export by d3d12.def). */
HRESULT __stdcall
npt_D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignature,
    ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob);

typedef struct TRITON12_SHADER
{
    void   *pDxbc;   /* full container: rebuilt DXBC, or verbatim DXIL */
    SIZE_T  cbDxbc;
    BOOL    IsDxil;  /* container carries a DXIL part (forwarded verbatim) */
    /* DXIL vertex shaders: the input signature, read once at create; NULL
     * when the program could not be read. */
    TritonDxilInputSig *pDxilInputs;
} TRITON12_SHADER, *PTRITON12_SHADER;

/* Container part scan: does a 'DXBC'-magic container carry a part with
 * this fourcc?  (DXIL containers reuse the DXBC container magic.) */
static BOOL
t12ContainerHasPart(const void *container, SIZE_T cb, UINT fourcc)
{
    const unsigned char *base = (const unsigned char *)container;
    if (cb < 32)
        return FALSE;
    UINT nBlob = *(const UINT *)(base + 28);
    const UINT *offs = (const UINT *)(base + 32);
    if (32 + (SIZE_T)nBlob * 4 > cb)
        return FALSE;
    for (UINT b = 0; b < nBlob; b++) {
        if ((SIZE_T)offs[b] + 8 > cb)
            continue;
        if (*(const UINT *)(base + offs[b]) == fourcc)
            return TRUE;
    }
    return FALSE;
}

typedef struct TRITON12_BLEND    { D3D12DDI_BLEND_DESC_0010 d; }        TRITON12_BLEND;
/* _0025 layout: the _0010 desc with DepthBoundsTestEnable appended.  The
 * R3 (_0022) table hands the shorter desc, which leaves the flag clear. */
typedef struct TRITON12_DS       { D3D12DDI_DEPTH_STENCIL_DESC_0025 d; } TRITON12_DS;
_Static_assert(offsetof(D3D12DDI_DEPTH_STENCIL_DESC_0025, DepthBoundsTestEnable) >=
               sizeof(D3D12DDI_DEPTH_STENCIL_DESC_0010), "_0025 extends _0010");
typedef struct TRITON12_RASTER   { D3D12DDI_RASTERIZER_DESC_0010 d; }   TRITON12_RASTER;
typedef struct TRITON12_ROOTSIG  { ID3D12RootSignature *pRS; }          TRITON12_ROOTSIG;
typedef struct TRITON12_PSO      { ID3D12PipelineState *pPSO; }         TRITON12_PSO;
typedef struct TRITON12_ELAYOUT
{
    UINT NumElements;
    D3D12DDIARG_INPUT_ELEMENT_DESC Elements[32];
} TRITON12_ELAYOUT;

/* ---------- shaders ---------- */

static SIZE_T APIENTRY
t12CalcPrivateShaderSize(D3D12DDI_HDEVICE hDevice,
                         const D3D12DDIARG_CREATE_SHADER_0010 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_SHADER);
}

static VOID
t12CreateShaderCommon(const D3D12DDIARG_CREATE_SHADER_0010 *pArgs,
                      D3D12DDI_HSHADER hShader, const char *tag,
                      int tessellation)
{
    PTRITON12_SHADER s = (PTRITON12_SHADER)hShader.pDrvPrivate;
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    if (!pArgs || !pArgs->pShaderCode)
        return;

    const UINT *tok = pArgs->pShaderCode;
    TR_LOG_HOT("12.CreateShader(%s): tok0=0x%08x tok1=0x%08x tok2=0x%08x "
           "tok3=0x%08x", tag, tok[0], tok[1], tok[2], tok[3]);

    if (tok[0] == 0x43425844u /* 'DXBC' container magic */) {
        /* Full container -- what the runtime hands a DXIL-consumer driver
         * for a DXIL app (a DXIL container reuses the DXBC magic).  The
         * length is ContainerSizeInBytes at byte offset 24; tok[1] is
         * digest bytes here, not a length.  Forward verbatim: the host
         * (Metal Shader Converter) takes DXIL natively, and D3DMetal
         * detects DXBC-vs-DXIL from the container itself. */
        UINT cb = tok[6];
        if (cb < 32 || cb > (128u << 20)) {
            TR_LOG("12.CreateShader(%s): container size %u INSANE", tag, cb);
            return;
        }
        void *copy = HeapAlloc(GetProcessHeap(), 0, cb);
        if (!copy)
            return;
        memcpy(copy, tok, cb);
        s->pDxbc = copy;
        s->cbDxbc = cb;
        s->IsDxil = t12ContainerHasPart(copy, cb, 0x4C495844u /* 'DXIL' */);
        TR_LOG_HOT("12.CreateShader(%s): container %u bytes verbatim (dxil=%d)",
               tag, cb, (int)s->IsDxil);
        return;
    }
    if (tok[2] == 0x4C495844u /* DxilProgramHeader: 'DXIL' at dword 2 */) {
        /* Bare DXIL program part -- what the runtime hands a DXIL-consumer
         * driver (sliced from the app's container for DXIL apps, fresh
         * dxilconv ConvertInDriver output for DXBC apps).  Wrap it in a
         * minimal container: header + lone DXIL part, digest zero.
         * The host compiles and executes a wrapped part correctly --
         * signatures come from the module's own metadata -- while a bare
         * part crashes it, so the wrap is mandatory. */
        SIZE_T cbPart = (SIZE_T)tok[1] * sizeof(UINT);
        if (cbPart < 16 || cbPart > (128u << 20)) {
            TR_LOG("12.CreateShader(%s): DXIL part size %zu INSANE", tag,
                   (size_t)cbPart);
            return;
        }
        SIZE_T cbPad = (cbPart + 3) & ~(SIZE_T)3;
        SIZE_T cb = 36 + 8 + cbPad; /* header + 1 offset + part hdr + data */
        unsigned char *c = (unsigned char *)HeapAlloc(GetProcessHeap(),
                                                      HEAP_ZERO_MEMORY, cb);
        if (!c)
            return;
        *(UINT *)(c + 0) = 0x43425844u;      /* 'DXBC' */
        *(USHORT *)(c + 20) = 1;             /* MajorVersion */
        *(UINT *)(c + 24) = (UINT)cb;        /* ContainerSizeInBytes */
        *(UINT *)(c + 28) = 1;               /* PartCount */
        *(UINT *)(c + 32) = 36;              /* PartOffset[0] */
        *(UINT *)(c + 36) = 0x4C495844u;     /* 'DXIL' */
        *(UINT *)(c + 40) = (UINT)cbPart;
        memcpy(c + 44, tok, cbPart);
        s->pDxbc = c;
        s->cbDxbc = cb;
        s->IsDxil = TRUE;
        TR_LOG_HOT("12.CreateShader(%s): wrapped bare DXIL part (%zu) into "
               "%zu-byte container", tag, (size_t)cbPart, (size_t)cb);
        return;
    }

    SIZE_T cbTok = (SIZE_T)tok[1] * sizeof(UINT);

    const void *pIn = NULL, *pOut = NULL, *pPatch = NULL;
    UINT nIn = 0, nOut = 0, nPatch = 0;
    if (!tessellation && pArgs->IOSignatures.Standard) {
        pIn  = pArgs->IOSignatures.Standard->pInputSignature;
        nIn  = pArgs->IOSignatures.Standard->NumInputSignatureEntries;
        pOut = pArgs->IOSignatures.Standard->pOutputSignature;
        nOut = pArgs->IOSignatures.Standard->NumOutputSignatureEntries;
    } else if (tessellation && pArgs->IOSignatures.Tessellation) {
        /* Hull/domain: the runtime hands the three-part tessellation
         * signature set.  Omitting it synthesized HS/DS containers with
         * no ISGN/OSGN/PCSG, and the host rejected every tessellation
         * PSO with E_INVALIDARG. */
        pIn    = pArgs->IOSignatures.Tessellation->pInputSignature;
        nIn    = pArgs->IOSignatures.Tessellation->NumInputSignatureEntries;
        pOut   = pArgs->IOSignatures.Tessellation->pOutputSignature;
        nOut   = pArgs->IOSignatures.Tessellation->NumOutputSignatureEntries;
        pPatch = pArgs->IOSignatures.Tessellation->pPatchConstantSignature;
        nPatch = pArgs->IOSignatures.Tessellation->NumPatchConstantSignatureEntries;
    }

    s->pDxbc = tritonBuildDxbc(tok, cbTok, pIn, nIn, pOut, nOut,
                               pPatch, nPatch,
                               sizeof(D3D12DDIARG_SIGNATURE_ENTRY_0012),
                               &s->cbDxbc);
    if (!s->pDxbc)
        TR_LOG("12.CreateShader(%s): container build FAILED tokens=%zu",
               tag, (size_t)cbTok);
}

/* A wrapped bare DXIL part carries its signatures only in the module
 * metadata.  The host binds input-layout elements to vertex inputs through
 * the container's ISGN/ISG1 part and silently drops every element it cannot
 * find there, so add an ISGN part built from the metadata: one entry per
 * register row, Register = StartRow (the attribute location of the DXIL
 * input). */
static void
t12AddDxilInputSigPart(PTRITON12_SHADER s)
{
    if (t12ContainerHasPart(s->pDxbc, s->cbDxbc, 0x4E475349u /* 'ISGN' */) ||
        t12ContainerHasPart(s->pDxbc, s->cbDxbc, 0x31475349u /* 'ISG1' */))
        return;
    size_t cbProgram = 0;
    const unsigned char *program = (const unsigned char *)
        tritonDxilFindProgram(s->pDxbc, s->cbDxbc, &cbProgram);
    if (!program)
        return;

    const char *names[32];
    UINT semIdx[32], regs[32], n = 0;
    const TritonDxilInputSig *sig = s->pDxilInputs;
    for (UINT i = 0; i < sig->Count; i++) {
        const TritonDxilInput *e = &sig->Elems[i];
        /* Generated inputs (SV_VertexID, SV_InstanceID) carry a system
         * semantic kind; a VS input named SV_Position is kind 0 and fetched
         * like any other attribute. */
        if (e->StartRow < 0 || e->SemanticKind != 0)
            continue;
        for (UINT r = 0; r < e->Rows && n < 32; r++, n++) {
            names[n] = e->Name;
            semIdx[n] = e->SemanticIndex[r];
            regs[n] = (UINT)e->StartRow + r;
        }
    }
    if (!n)
        return;

    SIZE_T cbSig = 0;
    unsigned char *sigDxbc = (unsigned char *)
        tritonBuildInputSigDxbc(names, semIdx, regs, n, &cbSig);
    if (!sigDxbc)
        return;
    const UINT sigOff = *(const UINT *)(sigDxbc + 32);
    const SIZE_T cbSigPart = cbSig - sigOff;   /* ISGN header + body */
    const SIZE_T cbDxilPart = 8 + cbProgram;   /* DXIL header + program */

    const SIZE_T cbHdr = 32 + 2 * 4;
    const SIZE_T cb = cbHdr + cbSigPart + cbDxilPart;
    unsigned char *c = (unsigned char *)HeapAlloc(GetProcessHeap(),
                                                  HEAP_ZERO_MEMORY, cb);
    if (!c) {
        HeapFree(GetProcessHeap(), 0, sigDxbc);
        return;
    }
    memcpy(c, s->pDxbc, 24);                    /* magic, digest, version */
    *(UINT *)(c + 24) = (UINT)cb;
    *(UINT *)(c + 28) = 2;
    *(UINT *)(c + 32) = (UINT)cbHdr;
    *(UINT *)(c + 36) = (UINT)(cbHdr + cbSigPart);
    memcpy(c + cbHdr, sigDxbc + sigOff, cbSigPart);
    memcpy(c + cbHdr + cbSigPart, program - 8, cbDxilPart);
    HeapFree(GetProcessHeap(), 0, sigDxbc);
    HeapFree(GetProcessHeap(), 0, s->pDxbc);
    s->pDxbc = c;
    s->cbDxbc = cb;
}

static VOID APIENTRY
t12CreateVertexShader(D3D12DDI_HDEVICE hDevice,
                      const D3D12DDIARG_CREATE_SHADER_0010 *pArgs,
                      D3D12DDI_HSHADER hShader)
{
    (void)hDevice;
    t12CreateShaderCommon(pArgs, hShader, "vs", 0);
    PTRITON12_SHADER s = (PTRITON12_SHADER)hShader.pDrvPrivate;
    if (!s || !s->IsDxil)
        return;
    /* The input-layout semantic names live only in the DXIL metadata; the
     * DDI hands registers.  Read them once for every PSO built on it. */
    size_t cbProgram = 0;
    const void *program = tritonDxilFindProgram(s->pDxbc, s->cbDxbc, &cbProgram);
    s->pDxilInputs = program ? tritonDxilReadInputSig(program, cbProgram) : NULL;
    if (!s->pDxilInputs)
        TR_LOG("12.CreateShader(vs): DXIL input signature unreadable "
               "(container %zu bytes)", (size_t)s->cbDxbc);
    else
        t12AddDxilInputSigPart(s);
}

static VOID APIENTRY
t12CreatePixelShader(D3D12DDI_HDEVICE hDevice,
                     const D3D12DDIARG_CREATE_SHADER_0010 *pArgs,
                     D3D12DDI_HSHADER hShader)
{ (void)hDevice; t12CreateShaderCommon(pArgs, hShader, "ps", 0); }

static VOID APIENTRY
t12CreateGeometryShader(D3D12DDI_HDEVICE hDevice,
                        const D3D12DDIARG_CREATE_SHADER_0010 *pArgs,
                        D3D12DDI_HSHADER hShader)
{ (void)hDevice; t12CreateShaderCommon(pArgs, hShader, "gs", 0); }

static VOID APIENTRY
t12CreateComputeShader(D3D12DDI_HDEVICE hDevice,
                       const D3D12DDIARG_CREATE_SHADER_0010 *pArgs,
                       D3D12DDI_HSHADER hShader)
{ (void)hDevice; t12CreateShaderCommon(pArgs, hShader, "cs", 0); }

static VOID APIENTRY
t12CreateHullShader(D3D12DDI_HDEVICE hDevice,
                    const D3D12DDIARG_CREATE_SHADER_0010 *pArgs,
                    D3D12DDI_HSHADER hShader)
{ (void)hDevice; t12CreateShaderCommon(pArgs, hShader, "hs", 1); }

static VOID APIENTRY
t12CreateDomainShader(D3D12DDI_HDEVICE hDevice,
                      const D3D12DDIARG_CREATE_SHADER_0010 *pArgs,
                      D3D12DDI_HSHADER hShader)
{ (void)hDevice; t12CreateShaderCommon(pArgs, hShader, "ds", 1); }

static VOID APIENTRY
t12DestroyShader(D3D12DDI_HDEVICE hDevice, D3D12DDI_HSHADER hShader)
{
    PTRITON12_SHADER s = (PTRITON12_SHADER)hShader.pDrvPrivate;
    (void)hDevice;
    if (s && s->pDxbc) {
        HeapFree(GetProcessHeap(), 0, s->pDxbc);
        s->pDxbc = NULL;
        s->cbDxbc = 0;
    }
    if (s) {
        tritonDxilFreeInputSig(s->pDxilInputs);
        s->pDxilInputs = NULL;
    }
}

/* ---------- fixed-function state objects (desc stores) ---------- */

static SIZE_T APIENTRY
t12CalcPrivateElementLayoutSize(D3D12DDI_HDEVICE hDevice,
                                const D3D12DDIARG_CREATEELEMENTLAYOUT_0010 *pArgs)
{ (void)hDevice; (void)pArgs; return sizeof(TRITON12_ELAYOUT); }

static VOID APIENTRY
t12CreateElementLayout(D3D12DDI_HDEVICE hDevice,
                       const D3D12DDIARG_CREATEELEMENTLAYOUT_0010 *pArgs,
                       D3D12DDI_HELEMENTLAYOUT hLayout)
{
    TRITON12_ELAYOUT *e = (TRITON12_ELAYOUT *)hLayout.pDrvPrivate;
    (void)hDevice;
    if (!e)
        return;
    memset(e, 0, sizeof(*e));
    if (!pArgs || !pArgs->pVertexElements)
        return;
    e->NumElements = pArgs->NumElements > 32 ? 32 : pArgs->NumElements;
    memcpy(e->Elements, pArgs->pVertexElements,
           e->NumElements * sizeof(e->Elements[0]));
    if (pArgs->NumElements > 32)
        TR_LOG("12.CreateElementLayout: %u elements TRUNCATED to 32",
               pArgs->NumElements);
}

static VOID APIENTRY
t12DestroyElementLayout(D3D12DDI_HDEVICE hDevice, D3D12DDI_HELEMENTLAYOUT h)
{ (void)hDevice; (void)h; }

static SIZE_T APIENTRY
t12CalcPrivateBlendStateSize(D3D12DDI_HDEVICE hDevice,
                             const D3D12DDI_BLEND_DESC_0010 *pDesc)
{ (void)hDevice; (void)pDesc; return sizeof(TRITON12_BLEND); }

static VOID APIENTRY
t12CreateBlendState(D3D12DDI_HDEVICE hDevice,
                    const D3D12DDI_BLEND_DESC_0010 *pDesc,
                    D3D12DDI_HBLENDSTATE hState)
{
    TRITON12_BLEND *b = (TRITON12_BLEND *)hState.pDrvPrivate;
    (void)hDevice;
    if (b && pDesc)
        b->d = *pDesc;
}

static VOID APIENTRY
t12DestroyBlendState(D3D12DDI_HDEVICE hDevice, D3D12DDI_HBLENDSTATE h)
{ (void)hDevice; (void)h; }

static SIZE_T APIENTRY
t12CalcPrivateDepthStencilStateSize(D3D12DDI_HDEVICE hDevice,
                                    const D3D12DDI_DEPTH_STENCIL_DESC_0010 *pDesc)
{ (void)hDevice; (void)pDesc; return sizeof(TRITON12_DS); }

static VOID APIENTRY
t12CreateDepthStencilState(D3D12DDI_HDEVICE hDevice,
                           const D3D12DDI_DEPTH_STENCIL_DESC_0010 *pDesc,
                           D3D12DDI_HDEPTHSTENCILSTATE hState)
{
    TRITON12_DS *s = (TRITON12_DS *)hState.pDrvPrivate;
    (void)hDevice;
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    if (pDesc)
        memcpy(&s->d, pDesc, sizeof(*pDesc));
}

static SIZE_T APIENTRY
t12CalcPrivateDepthStencilStateSize0025(D3D12DDI_HDEVICE hDevice,
                                        const D3D12DDI_DEPTH_STENCIL_DESC_0025 *pDesc)
{ (void)hDevice; (void)pDesc; return sizeof(TRITON12_DS); }

static VOID APIENTRY
t12CreateDepthStencilState0025(D3D12DDI_HDEVICE hDevice,
                               const D3D12DDI_DEPTH_STENCIL_DESC_0025 *pDesc,
                               D3D12DDI_HDEPTHSTENCILSTATE hState)
{
    TRITON12_DS *s = (TRITON12_DS *)hState.pDrvPrivate;
    (void)hDevice;
    if (s && pDesc)
        s->d = *pDesc;
}

static VOID APIENTRY
t12DestroyDepthStencilState(D3D12DDI_HDEVICE hDevice, D3D12DDI_HDEPTHSTENCILSTATE h)
{ (void)hDevice; (void)h; }

static SIZE_T APIENTRY
t12CalcPrivateRasterizerStateSize(D3D12DDI_HDEVICE hDevice,
                                  const D3D12DDI_RASTERIZER_DESC_0010 *pDesc)
{ (void)hDevice; (void)pDesc; return sizeof(TRITON12_RASTER); }

static VOID APIENTRY
t12CreateRasterizerState(D3D12DDI_HDEVICE hDevice,
                         const D3D12DDI_RASTERIZER_DESC_0010 *pDesc,
                         D3D12DDI_HRASTERIZERSTATE hState)
{
    TRITON12_RASTER *r = (TRITON12_RASTER *)hState.pDrvPrivate;
    (void)hDevice;
    if (r && pDesc)
        r->d = *pDesc;
}

static VOID APIENTRY
t12DestroyRasterizerState(D3D12DDI_HDEVICE hDevice, D3D12DDI_HRASTERIZERSTATE h)
{ (void)hDevice; (void)h; }

/* ---------- root signature ---------- */

static SIZE_T APIENTRY
t12CalcPrivateRootSignatureSize(D3D12DDI_HDEVICE hDevice,
                                const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 *pArgs)
{ (void)hDevice; (void)pArgs; return sizeof(TRITON12_ROOTSIG); }

/* Serialize vdesc over the wire and create the host root signature from
 * the blob, so the blob the host sees is always one it produced itself. */
static HRESULT
t12SerializeAndCreateRootSignature(PTRITON12_DEVICE p, UINT nodeMask,
                                   const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *vdesc,
                                   TRITON12_ROOTSIG *rs)
{
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = npt_D3D12SerializeVersionedRootSignature(vdesc, &blob, &err);
    if (FAILED(hr))
        TR_LOG("12.CreateRootSignature: version %d serialize failed 0x%08lx (%s)",
               (int)vdesc->Version, (unsigned long)hr,
               err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
    if (SUCCEEDED(hr) && blob)
        hr = ID3D12Device_CreateRootSignature(
            p->pDev, nodeMask, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
            (void **)&rs->pRS);
    if (blob) ID3D10Blob_Release(blob);
    if (err)  ID3D10Blob_Release(err);
    return hr;
}

/* The DDI hands the deserialized root signature in its 1.1 form, whose
 * structs mirror the API *1 structs field for field (range and descriptor
 * flags included), so it goes to the host as 1.1. */
static HRESULT APIENTRY
t12CreateRootSignature(D3D12DDI_HDEVICE hDevice,
                       const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 *pArgs,
                       D3D12DDI_HROOTSIGNATURE hRootSignature)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    TRITON12_ROOTSIG *rs = (TRITON12_ROOTSIG *)hRootSignature.pDrvPrivate;
    if (!p || !p->pDev || !rs || !pArgs || !pArgs->pRootSignature_1_1)
        return E_INVALIDARG;
    memset(rs, 0, sizeof(*rs));

    const D3D12DDI_ROOT_SIGNATURE_0013 *src = pArgs->pRootSignature_1_1;
    D3D12_ROOT_PARAMETER1 *params = NULL;
    D3D12_DESCRIPTOR_RANGE1 *ranges = NULL;
    D3D12_STATIC_SAMPLER_DESC *samplers = NULL;
    HRESULT hr = E_OUTOFMEMORY;

    UINT totalRanges = 0;
    for (UINT i = 0; i < src->NumParameters; i++)
        if (src->pRootParameters[i].ParameterType ==
            (D3D12DDI_ROOT_PARAMETER_TYPE)D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            totalRanges += src->pRootParameters[i].DescriptorTable.NumDescriptorRanges;

    if (src->NumParameters &&
        !(params = (D3D12_ROOT_PARAMETER1 *)calloc(src->NumParameters,
                                                   sizeof(*params))))
        goto out;
    if (totalRanges &&
        !(ranges = (D3D12_DESCRIPTOR_RANGE1 *)calloc(totalRanges,
                                                     sizeof(*ranges))))
        goto out;
    if (src->NumStaticSamplers &&
        !(samplers = (D3D12_STATIC_SAMPLER_DESC *)calloc(src->NumStaticSamplers,
                                                         sizeof(*samplers))))
        goto out;

    UINT ri = 0;
    for (UINT i = 0; i < src->NumParameters; i++) {
        const D3D12DDI_ROOT_PARAMETER_0013 *sp = &src->pRootParameters[i];
        D3D12_ROOT_PARAMETER1 *dp = &params[i];
        dp->ParameterType = (D3D12_ROOT_PARAMETER_TYPE)sp->ParameterType;
        dp->ShaderVisibility = (D3D12_SHADER_VISIBILITY)sp->ShaderVisibility;
        switch (dp->ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            UINT n = sp->DescriptorTable.NumDescriptorRanges;
            dp->DescriptorTable.NumDescriptorRanges = n;
            dp->DescriptorTable.pDescriptorRanges = &ranges[ri];
            for (UINT j = 0; j < n; j++) {
                const D3D12DDI_DESCRIPTOR_RANGE_0013 *sr =
                    &sp->DescriptorTable.pDescriptorRanges[j];
                D3D12_DESCRIPTOR_RANGE1 *dr = &ranges[ri + j];
                dr->RangeType = (D3D12_DESCRIPTOR_RANGE_TYPE)sr->RangeType;
                dr->NumDescriptors = sr->NumDescriptors;
                dr->BaseShaderRegister = sr->BaseShaderRegister;
                dr->RegisterSpace = sr->RegisterSpace;
                dr->Flags = (D3D12_DESCRIPTOR_RANGE_FLAGS)sr->Flags;
                dr->OffsetInDescriptorsFromTableStart =
                    sr->OffsetInDescriptorsFromTableStart;
            }
            ri += n;
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            dp->Constants.ShaderRegister = sp->Constants.ShaderRegister;
            dp->Constants.RegisterSpace = sp->Constants.RegisterSpace;
            dp->Constants.Num32BitValues = sp->Constants.Num32BitValues;
            break;
        default: /* CBV / SRV / UAV root descriptors */
            dp->Descriptor.ShaderRegister = sp->Descriptor.ShaderRegister;
            dp->Descriptor.RegisterSpace = sp->Descriptor.RegisterSpace;
            dp->Descriptor.Flags =
                (D3D12_ROOT_DESCRIPTOR_FLAGS)sp->Descriptor.Flags;
            break;
        }
    }
    /* D3D12DDI_STATIC_SAMPLER matches the API layout field for field. */
    for (UINT i = 0; i < src->NumStaticSamplers; i++)
        memcpy(&samplers[i], &src->pStaticSamplers[i], sizeof(samplers[i]));

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vdesc;
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vdesc.Desc_1_1.NumParameters = src->NumParameters;
    vdesc.Desc_1_1.pParameters = params;
    vdesc.Desc_1_1.NumStaticSamplers = src->NumStaticSamplers;
    vdesc.Desc_1_1.pStaticSamplers = samplers;
    /* The classic 1.0 set, LOCAL_ROOT_SIGNATURE (DXR shader-table
     * arguments) and the two *_HEAP_DIRECTLY_INDEXED bits. */
    vdesc.Desc_1_1.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)(src->Flags & 0xCFF);
    hr = t12SerializeAndCreateRootSignature(p, pArgs->NodeMask, &vdesc, rs);

out:
    TR_LOG_HR(hr, "12.CreateRootSignature: params=%u samplers=%u flags=0x%x -> 0x%08lx",
           src->NumParameters, src->NumStaticSamplers, (unsigned)src->Flags,
           (unsigned long)hr);
    free(params);
    free(ranges);
    free(samplers);
    return hr;
}

static VOID APIENTRY
t12DestroyRootSignature(D3D12DDI_HDEVICE hDevice, D3D12DDI_HROOTSIGNATURE h)
{
    TRITON12_ROOTSIG *rs = (TRITON12_ROOTSIG *)h.pDrvPrivate;
    (void)hDevice;
    if (rs && rs->pRS) {
        ID3D12RootSignature_Release(rs->pRS);
        rs->pRS = NULL;
    }
}

/* PSO / list-side accessors shared with tritonList12.c. */
ID3D12RootSignature *
triton12RootSig(D3D12DDI_HROOTSIGNATURE h)
{
    TRITON12_ROOTSIG *rs = (TRITON12_ROOTSIG *)h.pDrvPrivate;
    return rs ? rs->pRS : NULL;
}

ID3D12PipelineState *
triton12Pso(D3D12DDI_HPIPELINESTATE h)
{
    TRITON12_PSO *p = (TRITON12_PSO *)h.pDrvPrivate;
    return p ? p->pPSO : NULL;
}

/* ---------- PSO ---------- */

static SIZE_T APIENTRY
t12CalcPrivatePipelineStateSize(D3D12DDI_HDEVICE hDevice,
                                const D3D12DDIARG_CREATE_PIPELINE_STATE_0010 *pArgs)
{ (void)hDevice; (void)pArgs; return sizeof(TRITON12_PSO); }

static void
t12ShaderBytecode(D3D12DDI_HSHADER h, D3D12_SHADER_BYTECODE *out)
{
    PTRITON12_SHADER s = (PTRITON12_SHADER)h.pDrvPrivate;
    out->pShaderBytecode = (s && s->pDxbc) ? s->pDxbc : NULL;
    out->BytecodeLength  = (s && s->pDxbc) ? s->cbDxbc : 0;
}

/* Append one {type, payload} record to a pipeline-state stream, laid out
 * as CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT does: the record starts
 * pointer-aligned, the payload at its own alignment. */
static void
t12StreamPut(unsigned char *buf, SIZE_T *off,
             D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type,
             const void *payload, SIZE_T size, SIZE_T align)
{
    SIZE_T at = (*off + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
    memcpy(buf + at, &type, sizeof(type));
    at = (at + sizeof(type) + align - 1) & ~(align - 1);
    memcpy(buf + at, payload, size);
    *off = (at + size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
}

/* A graphics PSO the graphics-desc form cannot carry -- mesh/amplification
 * stages, or the depth-bounds enable that lives only in
 * D3D12_DEPTH_STENCIL_DESC1 -- goes to the host as a subobject stream
 * (ID3D12Device2::CreatePipelineState) built from the same assembled desc.
 * With ms set the mesh stages replace the vertex stages. */
static HRESULT
t12CreateStreamPipelineState(PTRITON12_DEVICE p,
                             const D3D12_GRAPHICS_PIPELINE_STATE_DESC *gd,
                             const D3D12_SHADER_BYTECODE *as,
                             const D3D12_SHADER_BYTECODE *ms,
                             BOOL depthBounds, TRITON12_PSO *pso)
{
    unsigned char buf[2048];
    SIZE_T off = 0;
#define PUT(tag, ptr, T) \
    t12StreamPut(buf, &off, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##tag, \
                 (ptr), sizeof(T), __alignof(T))
#define PUT_SHADER(tag, bc) \
    do { if ((bc)->pShaderBytecode) PUT(tag, (bc), D3D12_SHADER_BYTECODE); } while (0)
    PUT(ROOT_SIGNATURE, &gd->pRootSignature, ID3D12RootSignature *);
    if (ms) {
        PUT_SHADER(AS, as);
        PUT_SHADER(MS, ms);
    } else {
        PUT_SHADER(VS, &gd->VS);
        PUT_SHADER(HS, &gd->HS);
        PUT_SHADER(DS, &gd->DS);
        PUT_SHADER(GS, &gd->GS);
        if (gd->StreamOutput.NumEntries)
            PUT(STREAM_OUTPUT, &gd->StreamOutput, D3D12_STREAM_OUTPUT_DESC);
        if (gd->InputLayout.NumElements)
            PUT(INPUT_LAYOUT, &gd->InputLayout, D3D12_INPUT_LAYOUT_DESC);
        PUT(IB_STRIP_CUT_VALUE, &gd->IBStripCutValue,
            D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
    }
    PUT_SHADER(PS, &gd->PS);
    PUT(BLEND, &gd->BlendState, D3D12_BLEND_DESC);
    PUT(SAMPLE_MASK, &gd->SampleMask, UINT);
    PUT(RASTERIZER, &gd->RasterizerState, D3D12_RASTERIZER_DESC);
    if (depthBounds) {
        D3D12_DEPTH_STENCIL_DESC1 ds1;
        memset(&ds1, 0, sizeof(ds1));
        ds1.DepthEnable      = gd->DepthStencilState.DepthEnable;
        ds1.DepthWriteMask   = gd->DepthStencilState.DepthWriteMask;
        ds1.DepthFunc        = gd->DepthStencilState.DepthFunc;
        ds1.StencilEnable    = gd->DepthStencilState.StencilEnable;
        ds1.StencilReadMask  = gd->DepthStencilState.StencilReadMask;
        ds1.StencilWriteMask = gd->DepthStencilState.StencilWriteMask;
        ds1.FrontFace        = gd->DepthStencilState.FrontFace;
        ds1.BackFace         = gd->DepthStencilState.BackFace;
        ds1.DepthBoundsTestEnable = TRUE;
        PUT(DEPTH_STENCIL1, &ds1, D3D12_DEPTH_STENCIL_DESC1);
    } else {
        PUT(DEPTH_STENCIL, &gd->DepthStencilState, D3D12_DEPTH_STENCIL_DESC);
    }
    PUT(PRIMITIVE_TOPOLOGY, &gd->PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE);
    {
        struct D3D12_RT_FORMAT_ARRAY rts;
        memset(&rts, 0, sizeof(rts));
        rts.NumRenderTargets = gd->NumRenderTargets;
        for (UINT i = 0; i < 8; i++)
            rts.RTFormats[i] = gd->RTVFormats[i];
        t12StreamPut(buf, &off, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,
                     &rts, sizeof(rts), __alignof(UINT));
    }
    PUT(DEPTH_STENCIL_FORMAT, &gd->DSVFormat, DXGI_FORMAT);
    PUT(SAMPLE_DESC, &gd->SampleDesc, DXGI_SAMPLE_DESC);
    PUT(NODE_MASK, &gd->NodeMask, UINT);
#undef PUT_SHADER
#undef PUT

    ID3D12Device2 *dev2 = NULL;
    HRESULT hr = ID3D12Device_QueryInterface(p->pDev, &IID_ID3D12Device2,
                                             (void **)&dev2);
    if (FAILED(hr))
        return hr;
    D3D12_PIPELINE_STATE_STREAM_DESC sd;
    sd.SizeInBytes = off;
    sd.pPipelineStateSubobjectStream = buf;
    hr = ID3D12Device2_CreatePipelineState(dev2, &sd, &IID_ID3D12PipelineState,
                                           (void **)&pso->pPSO);
    ID3D12Device2_Release(dev2);
    TR_LOG_HR(hr, "12.CreatePipelineState(stream): mesh=%d bounds=%d rts=%u "
           "stream=%zu -> 0x%08lx", (int)(ms != NULL), (int)depthBounds,
           gd->NumRenderTargets, (size_t)off, (unsigned long)hr);
    return hr;
}

/* pArgs75 names the mesh/amplification stages when the runtime asked
 * through the _0075 slot; NULL from the _0022 table. */
static HRESULT
t12CreatePipelineStateImpl(D3D12DDI_HDEVICE hDevice,
                           const D3D12DDIARG_CREATE_PIPELINE_STATE_0010 *pArgs,
                           const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 *pArgs75,
                           D3D12DDI_HPIPELINESTATE hPipelineState,
                           D3D12DDI_HRTPIPELINESTATE hRTPipelineState)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    TRITON12_PSO *pso = (TRITON12_PSO *)hPipelineState.pDrvPrivate;
    (void)hRTPipelineState;
    if (!p || !p->pDev || !pso || !pArgs)
        return E_INVALIDARG;
    memset(pso, 0, sizeof(*pso));

    HRESULT hr;
    PTRITON12_SHADER cs = (PTRITON12_SHADER)pArgs->hComputeShader.pDrvPrivate;
    if (cs && cs->pDxbc) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
        memset(&cd, 0, sizeof(cd));
        cd.pRootSignature = triton12RootSig(pArgs->hRootSignature);
        cd.CS.pShaderBytecode = cs->pDxbc;
        cd.CS.BytecodeLength  = cs->cbDxbc;
        hr = ID3D12Device_CreateComputePipelineState(
            p->pDev, &cd, &IID_ID3D12PipelineState, (void **)&pso->pPSO);
        TR_LOG_HR(hr, "12.CreatePipelineState(compute) -> 0x%08lx", (unsigned long)hr);
        return hr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
    memset(&gd, 0, sizeof(gd));
    gd.pRootSignature = triton12RootSig(pArgs->hRootSignature);
    t12ShaderBytecode(pArgs->hVertexShader,   &gd.VS);
    t12ShaderBytecode(pArgs->hPixelShader,    &gd.PS);
    t12ShaderBytecode(pArgs->hDomainShader,   &gd.DS);
    t12ShaderBytecode(pArgs->hHullShader,     &gd.HS);
    t12ShaderBytecode(pArgs->hGeometryShader, &gd.GS);

    const TRITON12_BLEND *b = (const TRITON12_BLEND *)pArgs->hBlendState.pDrvPrivate;
    if (b) {
        gd.BlendState.AlphaToCoverageEnable  = b->d.AlphaToCoverageEnable;
        gd.BlendState.IndependentBlendEnable = b->d.IndependentBlendEnable;
        for (int i = 0; i < 8; i++) {
            const D3D12DDI_RENDER_TARGET_BLEND_DESC *sr = &b->d.RenderTarget[i];
            D3D12_RENDER_TARGET_BLEND_DESC *dr = &gd.BlendState.RenderTarget[i];
            dr->BlendEnable   = sr->BlendEnable;
            dr->LogicOpEnable = sr->LogicOpEnable;
            dr->SrcBlend      = (D3D12_BLEND)sr->SrcBlend;
            dr->DestBlend     = (D3D12_BLEND)sr->DestBlend;
            dr->BlendOp       = (D3D12_BLEND_OP)sr->BlendOp;
            dr->SrcBlendAlpha = (D3D12_BLEND)sr->SrcBlendAlpha;
            dr->DestBlendAlpha= (D3D12_BLEND)sr->DestBlendAlpha;
            dr->BlendOpAlpha  = (D3D12_BLEND_OP)sr->BlendOpAlpha;
            dr->LogicOp       = (D3D12_LOGIC_OP)sr->LogicOp;
            dr->RenderTargetWriteMask = sr->RenderTargetWriteMask;
        }
    } else {
        gd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
    }
    gd.SampleMask = pArgs->SampleMask;

    const TRITON12_RASTER *r = (const TRITON12_RASTER *)pArgs->hRasterizerState.pDrvPrivate;
    if (r) {
        gd.RasterizerState.FillMode = (D3D12_FILL_MODE)r->d.FillMode;
        gd.RasterizerState.CullMode = (D3D12_CULL_MODE)r->d.CullMode;
        gd.RasterizerState.FrontCounterClockwise = r->d.FrontCounterClockwise;
        gd.RasterizerState.DepthBias = r->d.DepthBias;
        gd.RasterizerState.DepthBiasClamp = r->d.DepthBiasClamp;
        gd.RasterizerState.SlopeScaledDepthBias = r->d.SlopeScaledDepthBias;
        gd.RasterizerState.DepthClipEnable = r->d.DepthClipEnable;
        gd.RasterizerState.MultisampleEnable = r->d.MultisampleEnable;
        gd.RasterizerState.AntialiasedLineEnable = r->d.AntialiasedLineEnable;
        gd.RasterizerState.ForcedSampleCount = r->d.ForcedSampleCount;
        gd.RasterizerState.ConservativeRaster =
            (D3D12_CONSERVATIVE_RASTERIZATION_MODE)r->d.ConservativeRasterizationMode;
    } else {
        gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        gd.RasterizerState.DepthClipEnable = TRUE;
    }

    const TRITON12_DS *ds = (const TRITON12_DS *)pArgs->hDepthStencilState.pDrvPrivate;
    if (ds) {
        gd.DepthStencilState.DepthEnable = ds->d.DepthEnable;
        gd.DepthStencilState.DepthWriteMask = (D3D12_DEPTH_WRITE_MASK)ds->d.DepthWriteMask;
        gd.DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)ds->d.DepthFunc;
        gd.DepthStencilState.StencilEnable = ds->d.StencilEnable;
        gd.DepthStencilState.StencilReadMask = ds->d.StencilReadMask;
        gd.DepthStencilState.StencilWriteMask = ds->d.StencilWriteMask;
        gd.DepthStencilState.FrontFace.StencilFailOp = (D3D12_STENCIL_OP)ds->d.FrontFace.StencilFailOp;
        gd.DepthStencilState.FrontFace.StencilDepthFailOp = (D3D12_STENCIL_OP)ds->d.FrontFace.StencilDepthFailOp;
        gd.DepthStencilState.FrontFace.StencilPassOp = (D3D12_STENCIL_OP)ds->d.FrontFace.StencilPassOp;
        gd.DepthStencilState.FrontFace.StencilFunc = (D3D12_COMPARISON_FUNC)ds->d.FrontFace.StencilFunc;
        gd.DepthStencilState.BackFace.StencilFailOp = (D3D12_STENCIL_OP)ds->d.BackFace.StencilFailOp;
        gd.DepthStencilState.BackFace.StencilDepthFailOp = (D3D12_STENCIL_OP)ds->d.BackFace.StencilDepthFailOp;
        gd.DepthStencilState.BackFace.StencilPassOp = (D3D12_STENCIL_OP)ds->d.BackFace.StencilPassOp;
        gd.DepthStencilState.BackFace.StencilFunc = (D3D12_COMPARISON_FUNC)ds->d.BackFace.StencilFunc;
    }

    /* Input layout: each element's InputRegister named from the stored VS
     * -- its synthesized ISGN for DXBC, its input-signature metadata for
     * DXIL. */
    D3D12_INPUT_ELEMENT_DESC elems[32];
    const TRITON12_ELAYOUT *el =
        (const TRITON12_ELAYOUT *)pArgs->hElementLayout.pDrvPrivate;
    PTRITON12_SHADER vsForIsgn =
        (PTRITON12_SHADER)pArgs->hVertexShader.pDrvPrivate;
    if (el && el->NumElements && vsForIsgn && vsForIsgn->pDxbc) {
        if (vsForIsgn->IsDxil && !vsForIsgn->pDxilInputs)
            return E_INVALIDARG;
        UINT n = 0;
        for (UINT i = 0; i < el->NumElements && n < 32; i++) {
            const D3D12DDIARG_INPUT_ELEMENT_DESC *se = &el->Elements[i];
            UINT semIdx = 0;
            const char *name;
            if (vsForIsgn->IsDxil) {
                name = tritonDxilInputSemantic(vsForIsgn->pDxilInputs,
                                               se->InputRegister, &semIdx);
            } else {
                name = tritonDxbcInputSemantic(
                    vsForIsgn->pDxbc, vsForIsgn->cbDxbc, se->InputRegister,
                    &semIdx);
            }
            if (!name)
                continue; /* register unused by this VS */
            elems[n].SemanticName = name;
            elems[n].SemanticIndex = semIdx;
            elems[n].Format = se->Format;
            elems[n].InputSlot = se->InputSlot;
            elems[n].AlignedByteOffset = se->AlignedByteOffset;
            elems[n].InputSlotClass =
                (D3D12_INPUT_CLASSIFICATION)se->InputSlotClass;
            elems[n].InstanceDataStepRate = se->InstanceDataStepRate;
            n++;
        }
        gd.InputLayout.pInputElementDescs = elems;
        gd.InputLayout.NumElements = n;
    }
    gd.IBStripCutValue = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)pArgs->IBStripCutValue;
    gd.PrimitiveTopologyType =
        (D3D12_PRIMITIVE_TOPOLOGY_TYPE)pArgs->PrimitiveTopologyType;
    gd.NumRenderTargets = pArgs->NumRenderTargets;
    for (int i = 0; i < 8; i++)
        gd.RTVFormats[i] = pArgs->RTVFormats[i];
    gd.DSVFormat = pArgs->DSVFormat;
    gd.SampleDesc = pArgs->SampleDesc;
    gd.NodeMask = pArgs->NodeMask;

    const BOOL depthBounds = ds && ds->d.DepthBoundsTestEnable;
    if (pArgs75 && (pArgs75->hMeshShader.pDrvPrivate ||
                    pArgs75->hAmplificationShader.pDrvPrivate)) {
        D3D12_SHADER_BYTECODE as, ms;
        t12ShaderBytecode(pArgs75->hAmplificationShader, &as);
        t12ShaderBytecode(pArgs75->hMeshShader, &ms);
        if (!ms.pShaderBytecode)
            return E_INVALIDARG;
        return t12CreateStreamPipelineState(p, &gd, &as, &ms, depthBounds, pso);
    }

    /* The host-backend vertex-fetch workaround the D3D11 input-layout path
     * applies: type the VS's integer inputs from the bound vertex formats. */
    void *pReconVs = NULL;
    if (el && vsForIsgn && vsForIsgn->pDxbc && !vsForIsgn->IsDxil &&
        (npt_host_workaround_flags() & NPT_WA_TYPE_VS_INPUT_FROM_VERTEX_FORMAT)) {
        unsigned char compReg[32] = { 0 };
        for (UINT i = 0; i < el->NumElements; i++)
            if (el->Elements[i].InputRegister < 32)
                compReg[el->Elements[i].InputRegister] =
                    tritonDxbcFormatCompType((UINT)el->Elements[i].Format);
        SIZE_T cbRecon = 0;
        pReconVs = tritonReconcileVsInputSig(vsForIsgn->pDxbc, vsForIsgn->cbDxbc,
                                             compReg, &cbRecon);
        if (pReconVs) {
            gd.VS.pShaderBytecode = pReconVs;
            gd.VS.BytecodeLength  = cbRecon;
        }
    }

    if (depthBounds) {
        hr = t12CreateStreamPipelineState(p, &gd, NULL, NULL, TRUE, pso);
    } else {
        hr = ID3D12Device_CreateGraphicsPipelineState(
            p->pDev, &gd, &IID_ID3D12PipelineState, (void **)&pso->pPSO);
        TR_LOG_HR(hr, "12.CreatePipelineState(graphics): rts=%u rtv0=%d dsv=%d "
               "samples=%u forced=%u -> 0x%08lx",
               pArgs->NumRenderTargets, (int)gd.RTVFormats[0], (int)gd.DSVFormat,
               gd.SampleDesc.Count, gd.RasterizerState.ForcedSampleCount,
               (unsigned long)hr);
    }
    if (pReconVs)
        HeapFree(GetProcessHeap(), 0, pReconVs);
    return hr;
}

static HRESULT APIENTRY
t12CreatePipelineState(D3D12DDI_HDEVICE hDevice,
                       const D3D12DDIARG_CREATE_PIPELINE_STATE_0010 *pArgs,
                       D3D12DDI_HPIPELINESTATE hPipelineState,
                       D3D12DDI_HRTPIPELINESTATE hRTPipelineState)
{
    return t12CreatePipelineStateImpl(hDevice, pArgs, NULL, hPipelineState,
                                      hRTPipelineState);
}

static VOID APIENTRY
t12DestroyPipelineState(D3D12DDI_HDEVICE hDevice, D3D12DDI_HPIPELINESTATE h)
{
    TRITON12_PSO *pso = (TRITON12_PSO *)h.pDrvPrivate;
    (void)hDevice;
    if (pso && pso->pPSO) {
        ID3D12PipelineState_Release(pso->pPSO);
        pso->pPSO = NULL;
    }
}

/* _0075 appends hMeshShader/hAmplificationShader to the PSO args; the
 * _0010 prefix the graphics path reads is unchanged. */
static SIZE_T APIENTRY
t12CalcPrivatePipelineStateSize0075(D3D12DDI_HDEVICE hDevice,
                                    const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 *pArgs)
{
    return t12CalcPrivatePipelineStateSize(
        hDevice, (const D3D12DDIARG_CREATE_PIPELINE_STATE_0010 *)pArgs);
}

static HRESULT APIENTRY
t12CreatePipelineState0075(D3D12DDI_HDEVICE hDevice,
                           const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 *pArgs,
                           D3D12DDI_HPIPELINESTATE hPso,
                           D3D12DDI_HRTPIPELINESTATE hRTPso)
{
    return t12CreatePipelineStateImpl(
        hDevice, (const D3D12DDIARG_CREATE_PIPELINE_STATE_0010 *)pArgs, pArgs,
        hPso, hRTPso);
}

/* _0026 keeps _0010's hRootSignature / pShaderCode / IOSignatures prefix
 * (the union gains a Mesh member), so the common store reads it as-is.
 * Mesh stages are DXIL-only, which the host takes verbatim; no
 * signature rebuild is involved. */
static VOID APIENTRY
t12CreateMeshShader(D3D12DDI_HDEVICE hDevice,
                    const D3D12DDIARG_CREATE_SHADER_0026 *pArgs,
                    D3D12DDI_HSHADER hShader)
{
    (void)hDevice;
    t12CreateShaderCommon((const D3D12DDIARG_CREATE_SHADER_0010 *)pArgs,
                          hShader, "ms", 0);
}

static VOID APIENTRY
t12CreateAmplificationShader(D3D12DDI_HDEVICE hDevice,
                             const D3D12DDIARG_CREATE_SHADER_0026 *pArgs,
                             D3D12DDI_HSHADER hShader)
{
    (void)hDevice;
    t12CreateShaderCommon((const D3D12DDIARG_CREATE_SHADER_0010 *)pArgs,
                          hShader, "as", 0);
}

static SIZE_T APIENTRY
t12CalcPrivateMeshShaderSize(D3D12DDI_HDEVICE hDevice,
                             const D3D12DDIARG_CREATE_SHADER_0026 *pArgs)
{ (void)hDevice; (void)pArgs; return sizeof(TRITON12_SHADER); }

/* Stream output is not implemented.  The shader still gets a zeroed private
 * struct: it shares pfnDestroyShader with every other stage, which frees
 * what the struct points at. */
static SIZE_T APIENTRY
t12CalcPrivateGsWithSoSize(D3D12DDI_HDEVICE hDevice,
                           const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0010 *pArgs)
{ (void)hDevice; (void)pArgs; return sizeof(TRITON12_SHADER); }

static VOID APIENTRY
t12CreateGsWithSo(D3D12DDI_HDEVICE hDevice,
                  const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0010 *pArgs,
                  D3D12DDI_HSHADER hShader)
{
    PTRITON12_SHADER s = (PTRITON12_SHADER)hShader.pDrvPrivate;
    (void)hDevice; (void)pArgs;
    if (s)
        memset(s, 0, sizeof(*s));
    TR_STUB("12.CreateGeometryShaderWithStreamOutput");
}

static SIZE_T APIENTRY
t12CalcPrivateGsWithSoSize0026(D3D12DDI_HDEVICE hDevice,
                               const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 *pArgs)
{ (void)hDevice; (void)pArgs; return sizeof(TRITON12_SHADER); }

static VOID APIENTRY
t12CreateGsWithSo0026(D3D12DDI_HDEVICE hDevice,
                      const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026 *pArgs,
                      D3D12DDI_HSHADER hShader)
{
    PTRITON12_SHADER s = (PTRITON12_SHADER)hShader.pDrvPrivate;
    (void)hDevice; (void)pArgs;
    if (s)
        memset(s, 0, sizeof(*s));
    TR_STUB("12.CreateGeometryShaderWithStreamOutput");
}

void
triton12InstallPipelineFuncs0080(D3D12DDI_DEVICE_FUNCS_CORE_0080 *t)
{
    t->pfnCalcPrivateDepthStencilStateSize = t12CalcPrivateDepthStencilStateSize0025;
    t->pfnCreateDepthStencilState          = t12CreateDepthStencilState0025;
    t->pfnCalcPrivatePipelineStateSize = t12CalcPrivatePipelineStateSize0075;
    t->pfnCreatePipelineState          = t12CreatePipelineState0075;
    t->pfnCreateAmplificationShader    = t12CreateAmplificationShader;
    t->pfnCreateMeshShader             = t12CreateMeshShader;
    t->pfnCalcPrivateMeshShaderSize    = t12CalcPrivateMeshShaderSize;
    t->pfnCalcPrivateGeometryShaderWithStreamOutput = t12CalcPrivateGsWithSoSize0026;
    t->pfnCreateGeometryShaderWithStreamOutput      = t12CreateGsWithSo0026;
}

void
triton12InstallPipelineFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnCalcPrivateElementLayoutSize     = t12CalcPrivateElementLayoutSize;
    t->pfnCreateElementLayout              = t12CreateElementLayout;
    t->pfnDestroyElementLayout             = t12DestroyElementLayout;
    t->pfnCalcPrivateBlendStateSize        = t12CalcPrivateBlendStateSize;
    t->pfnCreateBlendState                 = t12CreateBlendState;
    t->pfnDestroyBlendState                = t12DestroyBlendState;
    t->pfnCalcPrivateDepthStencilStateSize = t12CalcPrivateDepthStencilStateSize;
    t->pfnCreateDepthStencilState          = t12CreateDepthStencilState;
    t->pfnDestroyDepthStencilState         = t12DestroyDepthStencilState;
    t->pfnCalcPrivateRasterizerStateSize   = t12CalcPrivateRasterizerStateSize;
    t->pfnCreateRasterizerState            = t12CreateRasterizerState;
    t->pfnDestroyRasterizerState           = t12DestroyRasterizerState;
    t->pfnCalcPrivateShaderSize            = t12CalcPrivateShaderSize;
    t->pfnCreateVertexShader               = t12CreateVertexShader;
    t->pfnCreatePixelShader                = t12CreatePixelShader;
    t->pfnCreateGeometryShader             = t12CreateGeometryShader;
    t->pfnCalcPrivateGeometryShaderWithStreamOutput = t12CalcPrivateGsWithSoSize;
    t->pfnCreateGeometryShaderWithStreamOutput      = t12CreateGsWithSo;
    t->pfnCreateComputeShader              = t12CreateComputeShader;
    t->pfnCalcPrivateTessellationShaderSize = t12CalcPrivateShaderSize;
    t->pfnCreateHullShader                 = t12CreateHullShader;
    t->pfnCreateDomainShader               = t12CreateDomainShader;
    t->pfnDestroyShader                    = t12DestroyShader;
    t->pfnCalcPrivatePipelineStateSize     = t12CalcPrivatePipelineStateSize;
    t->pfnCreatePipelineState              = t12CreatePipelineState;
    t->pfnDestroyPipelineState             = t12DestroyPipelineState;
    t->pfnCalcPrivateRootSignatureSize     = t12CalcPrivateRootSignatureSize;
    t->pfnCreateRootSignature              = t12CreateRootSignature;
    t->pfnDestroyRootSignature             = t12DestroyRootSignature;
}
