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
 *    the API 1.0 desc and reuses the wire serialize path
 *    (npt_D3D12SerializeRootSignature) before inner CreateRootSignature;
 *  - PSO create assembles the API graphics/compute desc from the stored
 *    pieces and forwards to the inner device.
 */

#include "triton12.h"
#include "triton_log.h"

#include <stdio.h> /* snprintf (DXIL input-layout placeholder names) */

/* tritonDxbc.c */
void *tritonBuildDxbc(const UINT *pTokens, SIZE_T cbTokens,
                      const void *pInEntries,  UINT cInSigs,
                      const void *pOutEntries, UINT cOutSigs,
                      const void *pPatchEntries, UINT cPatchSigs,
                      UINT entryStride, SIZE_T *pcbOut);

/* npt_entry_d3d12.c (aliased to the public export by d3d12.def). */
HRESULT __stdcall
npt_D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
                                D3D_ROOT_SIGNATURE_VERSION Version,
                                ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob);
HRESULT __stdcall
npt_D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignature,
    ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob);

typedef struct TRITON12_SHADER
{
    void   *pDxbc;   /* full container: rebuilt DXBC, or verbatim DXIL */
    SIZE_T  cbDxbc;
    BOOL    IsDxil;  /* container carries a DXIL part (forwarded verbatim) */
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
typedef struct TRITON12_DS       { D3D12DDI_DEPTH_STENCIL_DESC_0010 d; } TRITON12_DS;
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
    TR_LOG("12.CreateShader(%s): tok0=0x%08x tok1=0x%08x tok2=0x%08x "
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
        TR_LOG("12.CreateShader(%s): container %u bytes verbatim (dxil=%d)",
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
        TR_LOG("12.CreateShader(%s): wrapped bare DXIL part (%zu) into "
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

static VOID APIENTRY
t12CreateVertexShader(D3D12DDI_HDEVICE hDevice,
                      const D3D12DDIARG_CREATE_SHADER_0010 *pArgs,
                      D3D12DDI_HSHADER hShader)
{ (void)hDevice; t12CreateShaderCommon(pArgs, hShader, "vs", 0); }

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

/* VERSION_1_1 create path: full-fidelity conversion of the DDI 1.1 desc
 * (the DDI structs mirror the API *1 structs field-for-field, including
 * range/descriptor flags) through the versioned wire serializer.  Used
 * for root signatures carrying the *_HEAP_DIRECTLY_INDEXED flags (SM 6.6
 * dynamic resources), which VERSION_1 serialization rejects. */
static HRESULT
t12CreateRootSignature11(PTRITON12_DEVICE p,
                         const D3D12DDI_ROOT_SIGNATURE_0013 *src,
                         TRITON12_ROOTSIG *rs)
{
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vdesc;
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vdesc.Desc_1_1.NumParameters = src->NumParameters;
    vdesc.Desc_1_1.NumStaticSamplers = src->NumStaticSamplers;
    /* Classic 1.0 set + the two HEAP_DIRECTLY_INDEXED bits. */
    vdesc.Desc_1_1.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)(src->Flags & 0xC7F);

    D3D12_ROOT_PARAMETER1 *params = NULL;
    D3D12_DESCRIPTOR_RANGE1 *ranges = NULL;
    D3D12_STATIC_SAMPLER_DESC *samplers = NULL;
    HRESULT hr = E_OUTOFMEMORY;

    UINT totalRanges = 0;
    for (UINT i = 0; i < src->NumParameters; i++)
        if (src->pRootParameters[i].ParameterType ==
            (D3D12DDI_ROOT_PARAMETER_TYPE)D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            totalRanges += src->pRootParameters[i].DescriptorTable.NumDescriptorRanges;

    if (src->NumParameters) {
        params = (D3D12_ROOT_PARAMETER1 *)calloc(src->NumParameters,
                                                 sizeof(*params));
        if (!params) goto out;
    }
    if (totalRanges) {
        ranges = (D3D12_DESCRIPTOR_RANGE1 *)calloc(totalRanges,
                                                   sizeof(*ranges));
        if (!ranges) goto out;
    }
    if (src->NumStaticSamplers) {
        samplers = (D3D12_STATIC_SAMPLER_DESC *)calloc(src->NumStaticSamplers,
                                                       sizeof(*samplers));
        if (!samplers) goto out;
    }

    {
        UINT ri = 0;
        for (UINT i = 0; i < src->NumParameters; i++) {
            const D3D12DDI_ROOT_PARAMETER_0013 *sp = &src->pRootParameters[i];
            D3D12_ROOT_PARAMETER1 *dp = &params[i];
            dp->ParameterType = (D3D12_ROOT_PARAMETER_TYPE)sp->ParameterType;
            dp->ShaderVisibility =
                (D3D12_SHADER_VISIBILITY)sp->ShaderVisibility;
            switch (dp->ParameterType) {
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
                UINT n = sp->DescriptorTable.NumDescriptorRanges;
                dp->DescriptorTable.NumDescriptorRanges = n;
                dp->DescriptorTable.pDescriptorRanges = &ranges[ri];
                for (UINT j = 0; j < n; j++) {
                    const D3D12DDI_DESCRIPTOR_RANGE_0013 *sr =
                        &sp->DescriptorTable.pDescriptorRanges[j];
                    D3D12_DESCRIPTOR_RANGE1 *dr = &ranges[ri + j];
                    dr->RangeType =
                        (D3D12_DESCRIPTOR_RANGE_TYPE)sr->RangeType;
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
                dp->Descriptor.ShaderRegister =
                    sp->Descriptor.ShaderRegister;
                dp->Descriptor.RegisterSpace = sp->Descriptor.RegisterSpace;
                dp->Descriptor.Flags =
                    (D3D12_ROOT_DESCRIPTOR_FLAGS)sp->Descriptor.Flags;
                break;
            }
        }
        for (UINT i = 0; i < src->NumStaticSamplers; i++)
            memcpy(&samplers[i], &src->pStaticSamplers[i],
                   sizeof(samplers[i]));
        vdesc.Desc_1_1.pParameters = params;
        vdesc.Desc_1_1.pStaticSamplers = samplers;
    }

    {
        ID3DBlob *blob = NULL, *err = NULL;
        hr = npt_D3D12SerializeVersionedRootSignature(&vdesc, &blob, &err);
        if (FAILED(hr))
            TR_LOG("12.CreateRootSignature11: serialize failed 0x%08lx (%s)",
                   (unsigned long)hr,
                   err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (SUCCEEDED(hr) && blob) {
            hr = ID3D12Device_CreateRootSignature(
                p->pDev, 0, ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                (void **)&rs->pRS);
        }
        if (blob) ID3D10Blob_Release(blob);
        if (err)  ID3D10Blob_Release(err);
    }
    TR_LOG("12.CreateRootSignature11: params=%u samplers=%u flags=0x%x -> "
           "0x%08lx", src->NumParameters, src->NumStaticSamplers,
           (unsigned)src->Flags, (unsigned long)hr);

out:
    free(params);
    free(ranges);
    free(samplers);
    return hr;
}

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

    /* SM 6.6 dynamic resources: a root signature the app indexes through
     * ResourceDescriptorHeap[] carries the 0x400/0x800
     * *_HEAP_DIRECTLY_INDEXED flags, which only VERSION_1_1
     * serialization accepts.  Route those through the versioned
     * serializer with full 1.1 fidelity (range/descriptor flags kept).
     * On failure fall through to the proven 1.0 down-convert, which
     * masks the bits off -- the pre-6.6 behavior (the runtime's INTERNAL
     * empty root signature also carries them and historically worked
     * masked). */
    if (src->Flags & 0xC00) {
        HRESULT hr11 = t12CreateRootSignature11(p, src, rs);
        if (SUCCEEDED(hr11))
            return hr11;
        TR_LOG("12.CreateRootSignature: 1_1 path failed 0x%08lx, "
               "falling back to masked 1_0", (unsigned long)hr11);
    }

    /* Convert the deserialized DDI 1.1 root signature to the API 1.0
     * desc (drop 1.1 optimization flags), serialize over the wire,
     * then hand the result to the inner CreateRootSignature, so the
     * blob the host sees is always one it produced itself. */
    D3D12_ROOT_SIGNATURE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.NumParameters = src->NumParameters;
    desc.NumStaticSamplers = src->NumStaticSamplers;
    /* VERSION_1 serialization rejects flag bits it doesn't know
     * (E_INVALIDARG), so keep only the classic 1.0 set:
     * ALLOW_IA | DENY_VS/HS/DS/GS/PS | ALLOW_SO. */
    desc.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)(src->Flags & 0x7F);

    D3D12_ROOT_PARAMETER *params = NULL;
    D3D12_DESCRIPTOR_RANGE *ranges = NULL;
    D3D12_STATIC_SAMPLER_DESC *samplers = NULL;
    HRESULT hr = E_OUTOFMEMORY;

    UINT totalRanges = 0;
    for (UINT i = 0; i < src->NumParameters; i++)
        if (src->pRootParameters[i].ParameterType ==
            (D3D12DDI_ROOT_PARAMETER_TYPE)D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            totalRanges += src->pRootParameters[i].DescriptorTable.NumDescriptorRanges;

    if (src->NumParameters) {
        params = (D3D12_ROOT_PARAMETER *)calloc(src->NumParameters, sizeof(*params));
        if (!params) goto out;
    }
    if (totalRanges) {
        ranges = (D3D12_DESCRIPTOR_RANGE *)calloc(totalRanges, sizeof(*ranges));
        if (!ranges) goto out;
    }
    if (src->NumStaticSamplers) {
        samplers = (D3D12_STATIC_SAMPLER_DESC *)calloc(src->NumStaticSamplers,
                                                       sizeof(*samplers));
        if (!samplers) goto out;
    }

    UINT ri = 0;
    for (UINT i = 0; i < src->NumParameters; i++) {
        const D3D12DDI_ROOT_PARAMETER_0013 *sp = &src->pRootParameters[i];
        D3D12_ROOT_PARAMETER *dp = &params[i];
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
                D3D12_DESCRIPTOR_RANGE *dr = &ranges[ri + j];
                dr->RangeType = (D3D12_DESCRIPTOR_RANGE_TYPE)sr->RangeType;
                dr->NumDescriptors = sr->NumDescriptors;
                dr->BaseShaderRegister = sr->BaseShaderRegister;
                dr->RegisterSpace = sr->RegisterSpace;
                dr->OffsetInDescriptorsFromTableStart =
                    sr->OffsetInDescriptorsFromTableStart;
            }
            ri += n;
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            dp->Constants.ShaderRegister = sp->Constants.ShaderRegister;
            dp->Constants.RegisterSpace  = sp->Constants.RegisterSpace;
            dp->Constants.Num32BitValues = sp->Constants.Num32BitValues;
            break;
        default: /* CBV / SRV / UAV root descriptors */
            dp->Descriptor.ShaderRegister = sp->Descriptor.ShaderRegister;
            dp->Descriptor.RegisterSpace  = sp->Descriptor.RegisterSpace;
            break;
        }
    }
    for (UINT i = 0; i < src->NumStaticSamplers; i++) {
        /* D3D12DDI_STATIC_SAMPLER matches the API layout field-for-field. */
        memcpy(&samplers[i], &src->pStaticSamplers[i], sizeof(samplers[i]));
    }
    desc.pParameters = params;
    desc.pStaticSamplers = samplers;

    {
        ID3DBlob *blob = NULL, *err = NULL;
        hr = npt_D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &blob, &err);
        if (FAILED(hr))
            TR_LOG("12.CreateRootSignature: serialize failed 0x%08lx (%s)",
                   (unsigned long)hr,
                   err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (SUCCEEDED(hr) && blob) {
            hr = ID3D12Device_CreateRootSignature(
                p->pDev, pArgs->NodeMask ? pArgs->NodeMask : 0,
                ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob),
                &IID_ID3D12RootSignature, (void **)&rs->pRS);
        }
        if (blob) ID3D10Blob_Release(blob);
        if (err)  ID3D10Blob_Release(err);
    }
    TR_LOG("12.CreateRootSignature: params=%u samplers=%u flags=0x%x -> 0x%08lx",
           src->NumParameters, src->NumStaticSamplers, (unsigned)src->Flags,
           (unsigned long)hr);

out:
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

/* Find the input-signature entry for a given input register inside a
 * container and return its semantic name/index.  Handles both signature
 * part layouts:
 *   ISGN -- classic DXBC (tritonBuildDxbc output): 24-byte elements,
 *           name offset at +0, semantic index at +4, register at +16;
 *   ISG1 -- DXIL containers and min-precision DXBC
 *           (DxilProgramSignatureElement, 32 bytes): Stream at +0, name
 *           offset at +4, semantic index at +8, register at +20.
 * Both encode string offsets relative to the part-data start.  The name
 * pointer aims INTO the container -- valid for the shader's lifetime. */
static const char *
t12SigSemanticForRegister(const void *dxbc, SIZE_T cb, UINT reg,
                          UINT *pSemIndex)
{
    const unsigned char *base = (const unsigned char *)dxbc;
    if (!base || cb < 32 || memcmp(base, "DXBC", 4) != 0)
        return NULL;
    UINT nBlob = *(const UINT *)(base + 28);
    const UINT *offs = (const UINT *)(base + 32);
    if (32 + (SIZE_T)nBlob * 4 > cb)
        return NULL;
    for (UINT b = 0; b < nBlob; b++) {
        if ((SIZE_T)offs[b] + 8 > cb)
            continue;
        const unsigned char *blob = base + offs[b];
        int isg1 = memcmp(blob, "ISG1", 4) == 0;
        if (!isg1 && memcmp(blob, "ISGN", 4) != 0)
            continue;
        const unsigned char *io = blob + 8; /* part data start */
        UINT n = *(const UINT *)io;         /* element count */
        UINT elOff = *(const UINT *)(io + 4);
        UINT stride = isg1 ? 32u : 24u;
        for (UINT i = 0; i < n; i++) {
            const unsigned char *el = io + elOff + i * stride;
            UINT nameOff, semIdx, r;
            if (isg1) {
                nameOff = *(const UINT *)(el + 4);
                semIdx  = *(const UINT *)(el + 8);
                r       = *(const UINT *)(el + 20);
            } else {
                nameOff = *(const UINT *)(el + 0);
                semIdx  = *(const UINT *)(el + 4);
                r       = *(const UINT *)(el + 16);
            }
            if (r == reg) {
                if (pSemIndex)
                    *pSemIndex = semIdx;
                return (const char *)(io + nameOff);
            }
        }
        return NULL;
    }
    return NULL;
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

static HRESULT APIENTRY
t12CreatePipelineState(D3D12DDI_HDEVICE hDevice,
                       const D3D12DDIARG_CREATE_PIPELINE_STATE_0010 *pArgs,
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
        TR_LOG("12.CreatePipelineState(compute) -> 0x%08lx", (unsigned long)hr);
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

    /* Input layout: semantics recovered from the stored VS container's
     * input signature -- ISGN in our synthesized DXBC, ISG1 in verbatim
     * DXIL containers (element InputRegister -> entry by register). */
    D3D12_INPUT_ELEMENT_DESC elems[32];
    char dxilNames[32][12]; /* "NPTA<reg>" placeholders, DXIL VS only */
    const TRITON12_ELAYOUT *el =
        (const TRITON12_ELAYOUT *)pArgs->hElementLayout.pDrvPrivate;
    PTRITON12_SHADER vsForIsgn =
        (PTRITON12_SHADER)pArgs->hVertexShader.pDrvPrivate;
    if (el && el->NumElements && vsForIsgn && vsForIsgn->pDxbc) {
        UINT n = 0;
        for (UINT i = 0; i < el->NumElements && n < 32; i++) {
            const D3D12DDIARG_INPUT_ELEMENT_DESC *se = &el->Elements[i];
            UINT semIdx = 0;
            const char *name;
            if (vsForIsgn->IsDxil) {
                /* Verbatim DXIL container: input-signature names live in
                 * the bitcode metadata, not in a readable ISG1.  Emit a
                 * register-tagged placeholder; the host render server
                 * resolves it to the real name via DXC reflection before
                 * the backend sees the desc (npt: NPTA rewrite). */
                snprintf(dxilNames[n], sizeof(dxilNames[n]), "NPTA%u",
                         se->InputRegister);
                name = dxilNames[n];
                semIdx = 0;
            } else {
                name = t12SigSemanticForRegister(
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

    hr = ID3D12Device_CreateGraphicsPipelineState(
        p->pDev, &gd, &IID_ID3D12PipelineState, (void **)&pso->pPSO);
    TR_LOG("12.CreatePipelineState(graphics): rts=%u -> 0x%08lx",
           pArgs->NumRenderTargets, (unsigned long)hr);
    return hr;
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

/* _0075 appends hMeshShader/hAmplificationShader to the PSO args; with
 * MeshShaderTier 0 the runtime never sets them and the _0010 prefix the
 * handlers read is unchanged. */
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
    if (pArgs && (pArgs->hMeshShader.pDrvPrivate ||
                  pArgs->hAmplificationShader.pDrvPrivate)) {
        TR_LOG("12.CreatePipelineState(0075): mesh pipeline requested with "
               "MeshShaderTier 0");
        return E_INVALIDARG;
    }
    return t12CreatePipelineState(
        hDevice, (const D3D12DDIARG_CREATE_PIPELINE_STATE_0010 *)pArgs, hPso,
        hRTPso);
}

static VOID APIENTRY
t12CreateMeshShaderUnsupported(D3D12DDI_HDEVICE hDevice,
                               const D3D12DDIARG_CREATE_SHADER_0026 *pArgs,
                               D3D12DDI_HSHADER hShader)
{
    PTRITON12_SHADER s = (PTRITON12_SHADER)hShader.pDrvPrivate;
    (void)hDevice; (void)pArgs;
    if (s)
        memset(s, 0, sizeof(*s));
    TR_STUB("12.CreateMesh/AmplificationShader");
}

static SIZE_T APIENTRY
t12CalcPrivateMeshShaderSize(D3D12DDI_HDEVICE hDevice,
                             const D3D12DDIARG_CREATE_SHADER_0026 *pArgs)
{ (void)hDevice; (void)pArgs; return sizeof(TRITON12_SHADER); }

void
triton12InstallPipelineFuncs0080(D3D12DDI_DEVICE_FUNCS_CORE_0080 *t)
{
    t->pfnCalcPrivatePipelineStateSize = t12CalcPrivatePipelineStateSize0075;
    t->pfnCreatePipelineState          = t12CreatePipelineState0075;
    t->pfnCreateAmplificationShader    = t12CreateMeshShaderUnsupported;
    t->pfnCreateMeshShader             = t12CreateMeshShaderUnsupported;
    t->pfnCalcPrivateMeshShaderSize    = t12CalcPrivateMeshShaderSize;
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
