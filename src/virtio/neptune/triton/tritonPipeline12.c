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

typedef struct TRITON12_SHADER
{
    void   *pDxbc;
    SIZE_T  cbDxbc;
} TRITON12_SHADER, *PTRITON12_SHADER;

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

    /* Convert the deserialized DDI 1.1 root signature to the API 1.0
     * desc (drop 1.1 optimization flags), serialize over the wire,
     * then hand the result to the inner CreateRootSignature, so the
     * blob the host sees is always one it produced itself. */
    D3D12_ROOT_SIGNATURE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.NumParameters = src->NumParameters;
    desc.NumStaticSamplers = src->NumStaticSamplers;
    /* VERSION_1 serialization rejects flag bits it doesn't know
     * (E_INVALIDARG).  The 26100 runtime's INTERNAL root signature (the
     * empty one it creates during device finalization) carries post-1.0
     * bits (e.g. 0x400/0x800 *_HEAP_DIRECTLY_INDEXED), so keep only the
     * classic 1.0 set: ALLOW_IA | DENY_VS/HS/DS/GS/PS | ALLOW_SO. */
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

/* Find the ISGN entry for a given input register inside one of OUR
 * synthesized containers (tritonBuildDxbc layout) and return its
 * semantic name/index.  The name pointer aims INTO the container --
 * valid for the shader object's lifetime. */
static const char *
t12IsgnSemanticForRegister(const void *dxbc, SIZE_T cb, UINT reg,
                           UINT *pSemIndex)
{
    const unsigned char *base = (const unsigned char *)dxbc;
    if (!base || cb < 32 || memcmp(base, "DXBC", 4) != 0)
        return NULL;
    UINT nBlob = *(const UINT *)(base + 28);
    const UINT *offs = (const UINT *)(base + 32);
    for (UINT b = 0; b < nBlob; b++) {
        if (offs[b] + 8 > cb)
            continue;
        const unsigned char *blob = base + offs[b];
        if (memcmp(blob, "ISGN", 4) != 0)
            continue;
        const unsigned char *io = blob + 8;
        UINT n = *(const UINT *)io;
        for (UINT i = 0; i < n; i++) {
            const unsigned char *el = io + 8 + i * 24;
            UINT nameOff = *(const UINT *)(el + 0);
            UINT semIdx  = *(const UINT *)(el + 4);
            UINT r       = *(const UINT *)(el + 16);
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

    /* Input layout: semantics recovered from OUR synthesized VS ISGN
     * (names/indices are ours by construction, so element InputRegister
     * -> ISGN entry by register). */
    D3D12_INPUT_ELEMENT_DESC elems[32];
    const TRITON12_ELAYOUT *el =
        (const TRITON12_ELAYOUT *)pArgs->hElementLayout.pDrvPrivate;
    PTRITON12_SHADER vsForIsgn =
        (PTRITON12_SHADER)pArgs->hVertexShader.pDrvPrivate;
    if (el && el->NumElements && vsForIsgn && vsForIsgn->pDxbc) {
        UINT n = 0;
        for (UINT i = 0; i < el->NumElements && n < 32; i++) {
            const D3D12DDIARG_INPUT_ELEMENT_DESC *se = &el->Elements[i];
            UINT semIdx = 0;
            const char *name = t12IsgnSemanticForRegister(
                vsForIsgn->pDxbc, vsForIsgn->cbDxbc, se->InputRegister,
                &semIdx);
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
