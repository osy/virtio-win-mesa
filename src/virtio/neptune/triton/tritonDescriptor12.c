/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 DDI descriptor heaps.  Descriptor handles are
 * host values passed through verbatim -- pfnCreateDescriptorHeap forwards
 * to the inner Neptune ID3D12Device, the heap-start handles and increment
 * come from the host (cached guest-side by npt_overrides_d3d12_device.c),
 * and every DestDescriptor.ptr the runtime hands us later is forwarded
 * unchanged.
 *
 * ABI note: the DDI heap-start getters return an 8-byte POD by value
 * (free-function ABI: RAX).  The inner COM methods return the same struct
 * through the C binding's explicit RetVal pointer (member-function ABI) --
 * call through lpVtbl with the two-arg shape, which is also how
 * npt_overrides_d3d12_device.c implements them.
 */

#include "triton12.h"
#include "triton_log.h"

/* D3D12DDI_DESCRIPTOR_HEAP_TYPE and D3D12_DESCRIPTOR_HEAP_TYPE agree
 * (CBV_SRV_UAV=0, SAMPLER=1, RTV=2, DSV=3). */

static SIZE_T APIENTRY
t12CalcPrivateDescriptorHeapSize(D3D12DDI_HDEVICE hDevice,
                                 const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_DESCRIPTOR_HEAP);
}

static HRESULT APIENTRY
t12CreateDescriptorHeap(D3D12DDI_HDEVICE hDevice,
                        const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *pArgs,
                        D3D12DDI_HDESCRIPTORHEAP hDescriptorHeap)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_DESCRIPTOR_HEAP h =
        (PTRITON12_DESCRIPTOR_HEAP)hDescriptorHeap.pDrvPrivate;
    if (!p || !p->pDev || !h || !pArgs)
        return E_INVALIDARG;
    memset(h, 0, sizeof(*h));

    D3D12_DESCRIPTOR_HEAP_DESC desc;
    desc.Type           = (D3D12_DESCRIPTOR_HEAP_TYPE)pArgs->Type;
    desc.NumDescriptors = pArgs->NumDescriptors;
    desc.Flags          = (pArgs->Flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
                            ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                            : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask       = 0;

    HRESULT hr = ID3D12Device_CreateDescriptorHeap(
        p->pDev, &desc, &IID_ID3D12DescriptorHeap, (void **)&h->pHeap);
    TR_LOG("12.CreateDescriptorHeap: type=%d n=%u ddiflags=0x%x -> 0x%08lx",
           (int)pArgs->Type, pArgs->NumDescriptors, (unsigned)pArgs->Flags,
           (unsigned long)hr);
    return hr;
}

static VOID APIENTRY
t12DestroyDescriptorHeap(D3D12DDI_HDEVICE hDevice,
                         D3D12DDI_HDESCRIPTORHEAP hDescriptorHeap)
{
    PTRITON12_DESCRIPTOR_HEAP h =
        (PTRITON12_DESCRIPTOR_HEAP)hDescriptorHeap.pDrvPrivate;
    (void)hDevice;
    if (h && h->pHeap) {
        ID3D12DescriptorHeap_Release(h->pHeap);
        h->pHeap = NULL;
    }
}

static UINT APIENTRY
t12GetDescriptorSizeInBytes(D3D12DDI_HDEVICE hDevice,
                            D3D12DDI_DESCRIPTOR_HEAP_TYPE Type)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    if (!p || !p->pDev)
        return 0;
    /* Answered from the guest-side increment cache after first fetch. */
    return ID3D12Device_GetDescriptorHandleIncrementSize(
        p->pDev, (D3D12_DESCRIPTOR_HEAP_TYPE)Type);
}

static D3D12DDI_CPU_DESCRIPTOR_HANDLE APIENTRY
t12GetCPUDescriptorHandleForHeapStart(D3D12DDI_HDEVICE hDevice,
                                      D3D12DDI_HDESCRIPTORHEAP hDescriptorHeap)
{
    D3D12DDI_CPU_DESCRIPTOR_HANDLE out;
    PTRITON12_DESCRIPTOR_HEAP h =
        (PTRITON12_DESCRIPTOR_HEAP)hDescriptorHeap.pDrvPrivate;
    (void)hDevice;
    out.ptr = 0;
    if (h && h->pHeap) {
        D3D12_CPU_DESCRIPTOR_HANDLE api;
        h->pHeap->lpVtbl->GetCPUDescriptorHandleForHeapStart(h->pHeap, &api);
        out.ptr = api.ptr;
    }
    return out;
}

static D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY
t12GetGPUDescriptorHandleForHeapStart(D3D12DDI_HDEVICE hDevice,
                                      D3D12DDI_HDESCRIPTORHEAP hDescriptorHeap)
{
    D3D12DDI_GPU_DESCRIPTOR_HANDLE out;
    PTRITON12_DESCRIPTOR_HEAP h =
        (PTRITON12_DESCRIPTOR_HEAP)hDescriptorHeap.pDrvPrivate;
    (void)hDevice;
    out.ptr = 0;
    if (h && h->pHeap) {
        D3D12_GPU_DESCRIPTOR_HANDLE api;
        h->pHeap->lpVtbl->GetGPUDescriptorHandleForHeapStart(h->pHeap, &api);
        out.ptr = api.ptr;
    }
    return out;
}

/* ---------- view creates (RTV first; the rest as probes demand) ---------- */

static VOID APIENTRY
t12CreateRenderTargetView(D3D12DDI_HDEVICE hDevice,
                          const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 *pArgs,
                          D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_RESOURCE r =
        pArgs ? (PTRITON12_RESOURCE)pArgs->hDrvResource.pDrvPrivate : NULL;
    if (!p || !p->pDev || !pArgs)
        return;

    /* DestDescriptor.ptr is the HOST cpu-descriptor value the runtime
     * computed from our heap-start + host increment: pass verbatim. */
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    dst.ptr = (SIZE_T)DestDescriptor.ptr;

    D3D12_RENDER_TARGET_VIEW_DESC vd;
    memset(&vd, 0, sizeof(vd));
    vd.Format = pArgs->Format;
    const BOOL msaa = r && r->Desc.SampleDesc.Count > 1;
    switch (pArgs->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        vd.ViewDimension = D3D12_RTV_DIMENSION_BUFFER;
        vd.Buffer.FirstElement = pArgs->Buffer.FirstElement;
        vd.Buffer.NumElements  = pArgs->Buffer.NumElements;
        break;
    case D3D12DDI_RD_TEXTURE1D:
        if (pArgs->Tex1D.ArraySize > 1 || pArgs->Tex1D.FirstArraySlice) {
            vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
            vd.Texture1DArray.MipSlice = pArgs->Tex1D.MipSlice;
            vd.Texture1DArray.FirstArraySlice = pArgs->Tex1D.FirstArraySlice;
            vd.Texture1DArray.ArraySize = pArgs->Tex1D.ArraySize;
        } else {
            vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
            vd.Texture1D.MipSlice = pArgs->Tex1D.MipSlice;
        }
        break;
    case D3D12DDI_RD_TEXTURE3D:
        vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        vd.Texture3D.MipSlice = pArgs->Tex3D.MipSlice;
        vd.Texture3D.FirstWSlice = pArgs->Tex3D.FirstW;
        vd.Texture3D.WSize = pArgs->Tex3D.WSize;
        break;
    case D3D12DDI_RD_TEXTURE2D:
    default:
        if (msaa) {
            if (pArgs->Tex2D.ArraySize > 1 || pArgs->Tex2D.FirstArraySlice) {
                vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                vd.Texture2DMSArray.FirstArraySlice = pArgs->Tex2D.FirstArraySlice;
                vd.Texture2DMSArray.ArraySize = pArgs->Tex2D.ArraySize;
            } else {
                vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
            }
        } else if (pArgs->Tex2D.ArraySize > 1 || pArgs->Tex2D.FirstArraySlice) {
            vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipSlice = pArgs->Tex2D.MipSlice;
            vd.Texture2DArray.FirstArraySlice = pArgs->Tex2D.FirstArraySlice;
            vd.Texture2DArray.ArraySize = pArgs->Tex2D.ArraySize;
            vd.Texture2DArray.PlaneSlice = pArgs->Tex2D.PlaneSlice;
        } else {
            vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = pArgs->Tex2D.MipSlice;
            vd.Texture2D.PlaneSlice = pArgs->Tex2D.PlaneSlice;
        }
        break;
    }
    /* A NULL resource is a null view, which is how an app clears a heap
     * slot.  It must still be written: skipping it leaves the slot naming
     * whatever it named before, which may since have been destroyed. */
    ID3D12Device_CreateRenderTargetView(p->pDev, r ? r->pResource : NULL,
                                        &vd, dst);
}


/* ---------- remaining view creates (P6) ---------- */

static VOID APIENTRY
t12CreateShaderResourceView(D3D12DDI_HDEVICE hDevice,
                            const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 *pArgs,
                            D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_RESOURCE r =
        pArgs ? (PTRITON12_RESOURCE)pArgs->hDrvResource.pDrvPrivate : NULL;
    if (!p || !p->pDev || !pArgs)
        return;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    dst.ptr = (SIZE_T)DestDescriptor.ptr;

    D3D12_SHADER_RESOURCE_VIEW_DESC vd;
    memset(&vd, 0, sizeof(vd));
    vd.Format = pArgs->Format;
    vd.Shader4ComponentMapping = pArgs->Shader4ComponentMapping;
    const BOOL msaa = r && r->Desc.SampleDesc.Count > 1;
    switch (pArgs->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        vd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        vd.Buffer.FirstElement = pArgs->Buffer.FirstElement;
        vd.Buffer.NumElements = pArgs->Buffer.NumElements;
        vd.Buffer.StructureByteStride = pArgs->Buffer.StructureByteStride;
        vd.Buffer.Flags = (D3D12_BUFFER_SRV_FLAGS)pArgs->Buffer.Flags;
        break;
    case D3D12DDI_RD_TEXTURE1D:
        if (pArgs->Tex1D.ArraySize > 1 || pArgs->Tex1D.FirstArraySlice) {
            vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            vd.Texture1DArray.MostDetailedMip = pArgs->Tex1D.MostDetailedMip;
            vd.Texture1DArray.MipLevels = pArgs->Tex1D.MipLevels;
            vd.Texture1DArray.FirstArraySlice = pArgs->Tex1D.FirstArraySlice;
            vd.Texture1DArray.ArraySize = pArgs->Tex1D.ArraySize;
            vd.Texture1DArray.ResourceMinLODClamp = pArgs->Tex1D.ResourceMinLODClamp;
        } else {
            vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            vd.Texture1D.MostDetailedMip = pArgs->Tex1D.MostDetailedMip;
            vd.Texture1D.MipLevels = pArgs->Tex1D.MipLevels;
            vd.Texture1D.ResourceMinLODClamp = pArgs->Tex1D.ResourceMinLODClamp;
        }
        break;
    case D3D12DDI_RD_TEXTURE3D:
        vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        vd.Texture3D.MostDetailedMip = pArgs->Tex3D.MostDetailedMip;
        vd.Texture3D.MipLevels = pArgs->Tex3D.MipLevels;
        vd.Texture3D.ResourceMinLODClamp = pArgs->Tex3D.ResourceMinLODClamp;
        break;
    case D3D12DDI_RD_TEXTURECUBE:
        if (pArgs->TexCube.NumCubes > 1 || pArgs->TexCube.First2DArrayFace) {
            vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            vd.TextureCubeArray.MostDetailedMip = pArgs->TexCube.MostDetailedMip;
            vd.TextureCubeArray.MipLevels = pArgs->TexCube.MipLevels;
            vd.TextureCubeArray.First2DArrayFace = pArgs->TexCube.First2DArrayFace;
            vd.TextureCubeArray.NumCubes = pArgs->TexCube.NumCubes;
            vd.TextureCubeArray.ResourceMinLODClamp = pArgs->TexCube.ResourceMinLODClamp;
        } else {
            vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            vd.TextureCube.MostDetailedMip = pArgs->TexCube.MostDetailedMip;
            vd.TextureCube.MipLevels = pArgs->TexCube.MipLevels;
            vd.TextureCube.ResourceMinLODClamp = pArgs->TexCube.ResourceMinLODClamp;
        }
        break;
    case D3D12DDI_RD_TEXTURE2D:
    default:
        if (msaa) {
            if (pArgs->Tex2D.ArraySize > 1 || pArgs->Tex2D.FirstArraySlice) {
                vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                vd.Texture2DMSArray.FirstArraySlice = pArgs->Tex2D.FirstArraySlice;
                vd.Texture2DMSArray.ArraySize = pArgs->Tex2D.ArraySize;
            } else {
                vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            }
        } else if (pArgs->Tex2D.ArraySize > 1 || pArgs->Tex2D.FirstArraySlice) {
            vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MostDetailedMip = pArgs->Tex2D.MostDetailedMip;
            vd.Texture2DArray.MipLevels = pArgs->Tex2D.MipLevels;
            vd.Texture2DArray.FirstArraySlice = pArgs->Tex2D.FirstArraySlice;
            vd.Texture2DArray.ArraySize = pArgs->Tex2D.ArraySize;
            vd.Texture2DArray.PlaneSlice = pArgs->Tex2D.PlaneSlice;
            vd.Texture2DArray.ResourceMinLODClamp = pArgs->Tex2D.ResourceMinLODClamp;
        } else {
            vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MostDetailedMip = pArgs->Tex2D.MostDetailedMip;
            vd.Texture2D.MipLevels = pArgs->Tex2D.MipLevels;
            vd.Texture2D.PlaneSlice = pArgs->Tex2D.PlaneSlice;
            vd.Texture2D.ResourceMinLODClamp = pArgs->Tex2D.ResourceMinLODClamp;
        }
        break;
    }
    ID3D12Device_CreateShaderResourceView(
        p->pDev, r ? r->pResource : NULL, &vd, dst);
}

static VOID APIENTRY
t12CreateConstantBufferView(D3D12DDI_HDEVICE hDevice,
                            const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC *pDesc,
                            D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    if (!p || !p->pDev || !pDesc)
        return;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    dst.ptr = (SIZE_T)DestDescriptor.ptr;
    D3D12_CONSTANT_BUFFER_VIEW_DESC vd;
    vd.BufferLocation = pDesc->BufferLocation; /* host VA, verbatim */
    vd.SizeInBytes = pDesc->SizeInBytes;
    ID3D12Device_CreateConstantBufferView(p->pDev, &vd, dst);
}

static VOID APIENTRY
t12CreateSampler(D3D12DDI_HDEVICE hDevice,
                 const D3D12DDIARG_CREATE_SAMPLER *pArgs,
                 D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    if (!p || !p->pDev || !pArgs || !pArgs->pSamplerDesc)
        return;
    const D3D12DDI_SAMPLER_DESC *sd = pArgs->pSamplerDesc;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    dst.ptr = (SIZE_T)DestDescriptor.ptr;
    D3D12_SAMPLER_DESC vd;
    memset(&vd, 0, sizeof(vd));
    vd.Filter = (D3D12_FILTER)sd->Filter;
    vd.AddressU = (D3D12_TEXTURE_ADDRESS_MODE)sd->AddressU;
    vd.AddressV = (D3D12_TEXTURE_ADDRESS_MODE)sd->AddressV;
    vd.AddressW = (D3D12_TEXTURE_ADDRESS_MODE)sd->AddressW;
    vd.MipLODBias = sd->MipLODBias;
    vd.MaxAnisotropy = sd->MaxAnisotropy;
    vd.ComparisonFunc = (D3D12_COMPARISON_FUNC)sd->ComparisonFunc;
    memcpy(vd.BorderColor, sd->BorderColor, sizeof(vd.BorderColor));
    vd.MinLOD = sd->MinLOD;
    vd.MaxLOD = sd->MaxLOD;
    ID3D12Device_CreateSampler(p->pDev, &vd, dst);
}

static VOID APIENTRY
t12CreateUnorderedAccessView(D3D12DDI_HDEVICE hDevice,
                             const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 *pArgs,
                             D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_RESOURCE r =
        pArgs ? (PTRITON12_RESOURCE)pArgs->hDrvResource.pDrvPrivate : NULL;
    if (!p || !p->pDev || !pArgs)
        return;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    dst.ptr = (SIZE_T)DestDescriptor.ptr;

    ID3D12Resource *counter = NULL;
    D3D12_UNORDERED_ACCESS_VIEW_DESC vd;
    memset(&vd, 0, sizeof(vd));
    vd.Format = pArgs->Format;
    switch (pArgs->ResourceDimension) {
    case D3D12DDI_RD_BUFFER: {
        PTRITON12_RESOURCE c =
            (PTRITON12_RESOURCE)pArgs->Buffer.hDrvCounterResource.pDrvPrivate;
        counter = (c && c->pResource) ? c->pResource : NULL;
        vd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        vd.Buffer.FirstElement = pArgs->Buffer.FirstElement;
        vd.Buffer.NumElements = pArgs->Buffer.NumElements;
        vd.Buffer.StructureByteStride = pArgs->Buffer.StructureByteStride;
        vd.Buffer.CounterOffsetInBytes = pArgs->Buffer.CounterOffsetInBytes;
        vd.Buffer.Flags = (D3D12_BUFFER_UAV_FLAGS)pArgs->Buffer.Flags;
        break;
    }
    case D3D12DDI_RD_TEXTURE1D:
        if (pArgs->Tex1D.ArraySize > 1 || pArgs->Tex1D.FirstArraySlice) {
            vd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            vd.Texture1DArray.MipSlice = pArgs->Tex1D.MipSlice;
            vd.Texture1DArray.FirstArraySlice = pArgs->Tex1D.FirstArraySlice;
            vd.Texture1DArray.ArraySize = pArgs->Tex1D.ArraySize;
        } else {
            vd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            vd.Texture1D.MipSlice = pArgs->Tex1D.MipSlice;
        }
        break;
    case D3D12DDI_RD_TEXTURE3D:
        vd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        vd.Texture3D.MipSlice = pArgs->Tex3D.MipSlice;
        vd.Texture3D.FirstWSlice = pArgs->Tex3D.FirstW;
        vd.Texture3D.WSize = pArgs->Tex3D.WSize;
        break;
    case D3D12DDI_RD_TEXTURE2D:
    default:
        if (pArgs->Tex2D.ArraySize > 1 || pArgs->Tex2D.FirstArraySlice) {
            vd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipSlice = pArgs->Tex2D.MipSlice;
            vd.Texture2DArray.FirstArraySlice = pArgs->Tex2D.FirstArraySlice;
            vd.Texture2DArray.ArraySize = pArgs->Tex2D.ArraySize;
            vd.Texture2DArray.PlaneSlice = pArgs->Tex2D.PlaneSlice;
        } else {
            vd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = pArgs->Tex2D.MipSlice;
            vd.Texture2D.PlaneSlice = pArgs->Tex2D.PlaneSlice;
        }
        break;
    }
    ID3D12Device_CreateUnorderedAccessView(
        p->pDev, r ? r->pResource : NULL, counter, &vd, dst);
}

static VOID APIENTRY
t12CreateDepthStencilView(D3D12DDI_HDEVICE hDevice,
                          const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW *pArgs,
                          D3D12DDI_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_RESOURCE r =
        pArgs ? (PTRITON12_RESOURCE)pArgs->hDrvResource.pDrvPrivate : NULL;
    if (!p || !p->pDev || !pArgs)
        return;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    dst.ptr = (SIZE_T)DestDescriptor.ptr;

    D3D12_DEPTH_STENCIL_VIEW_DESC vd;
    memset(&vd, 0, sizeof(vd));
    vd.Format = pArgs->Format;
    vd.Flags = (D3D12_DSV_FLAGS)pArgs->Flags;
    const BOOL msaa = r && r->Desc.SampleDesc.Count > 1;
    switch (pArgs->ResourceDimension) {
    case D3D12DDI_RD_TEXTURE1D:
        if (pArgs->Tex1D.ArraySize > 1 || pArgs->Tex1D.FirstArraySlice) {
            vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
            vd.Texture1DArray.MipSlice = pArgs->Tex1D.MipSlice;
            vd.Texture1DArray.FirstArraySlice = pArgs->Tex1D.FirstArraySlice;
            vd.Texture1DArray.ArraySize = pArgs->Tex1D.ArraySize;
        } else {
            vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
            vd.Texture1D.MipSlice = pArgs->Tex1D.MipSlice;
        }
        break;
    case D3D12DDI_RD_TEXTURE2D:
    case D3D12DDI_RD_TEXTURECUBE:
    default:
        if (msaa) {
            if (pArgs->Tex2D.ArraySize > 1 || pArgs->Tex2D.FirstArraySlice) {
                vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                vd.Texture2DMSArray.FirstArraySlice = pArgs->Tex2D.FirstArraySlice;
                vd.Texture2DMSArray.ArraySize = pArgs->Tex2D.ArraySize;
            } else {
                vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
            }
        } else if (pArgs->Tex2D.ArraySize > 1 || pArgs->Tex2D.FirstArraySlice) {
            vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipSlice = pArgs->Tex2D.MipSlice;
            vd.Texture2DArray.FirstArraySlice = pArgs->Tex2D.FirstArraySlice;
            vd.Texture2DArray.ArraySize = pArgs->Tex2D.ArraySize;
        } else {
            vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = pArgs->Tex2D.MipSlice;
        }
        break;
    }
    /* NULL resource is a null view, as for render targets above. */
    ID3D12Device_CreateDepthStencilView(p->pDev, r ? r->pResource : NULL,
                                        &vd, dst);
}

static VOID APIENTRY
t12CopyDescriptors(D3D12DDI_HDEVICE hDevice, UINT NumDest,
                   const D3D12DDI_CPU_DESCRIPTOR_HANDLE *pDestStarts,
                   const UINT *pDestSizes, UINT NumSrc,
                   const D3D12DDI_CPU_DESCRIPTOR_HANDLE *pSrcStarts,
                   const UINT *pSrcSizes,
                   D3D12DDI_DESCRIPTOR_HEAP_TYPE Type)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    if (!p || !p->pDev)
        return;
    /* Handles are host values -- forward arrays verbatim (layout-identical
     * 8-byte structs). */
    ID3D12Device_CopyDescriptors(
        p->pDev, NumDest, (const D3D12_CPU_DESCRIPTOR_HANDLE *)pDestStarts,
        pDestSizes, NumSrc, (const D3D12_CPU_DESCRIPTOR_HANDLE *)pSrcStarts,
        pSrcSizes, (D3D12_DESCRIPTOR_HEAP_TYPE)Type);
}

static VOID APIENTRY
t12CopyDescriptorsSimple(D3D12DDI_HDEVICE hDevice, UINT Num,
                         D3D12DDI_CPU_DESCRIPTOR_HANDLE Dest,
                         D3D12DDI_CPU_DESCRIPTOR_HANDLE Src,
                         D3D12DDI_DESCRIPTOR_HEAP_TYPE Type)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    if (!p || !p->pDev)
        return;
    D3D12_CPU_DESCRIPTOR_HANDLE d, sh;
    d.ptr = (SIZE_T)Dest.ptr;
    sh.ptr = (SIZE_T)Src.ptr;
    ID3D12Device_CopyDescriptorsSimple(
        p->pDev, Num, d, sh, (D3D12_DESCRIPTOR_HEAP_TYPE)Type);
}

static D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY
t12CheckResourceVirtualAddress(D3D12DDI_HDEVICE hDevice,
                               D3D12DDI_HRESOURCE hResource)
{
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    (void)hDevice;
    if (!r || !r->pResource)
        return 0;
    /* Host VAs pass through verbatim.  Cached because the wrapper call
     * is a wire round trip. */
    if (!r->GpuVa)
        r->GpuVa = ID3D12Resource_GetGPUVirtualAddress(r->pResource);
    return r->GpuVa;
}

void
triton12InstallDescriptorFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnCreateRenderTargetView               = t12CreateRenderTargetView;
    t->pfnCreateShaderResourceView             = t12CreateShaderResourceView;
    t->pfnCreateConstantBufferView             = t12CreateConstantBufferView;
    t->pfnCreateSampler                        = t12CreateSampler;
    t->pfnCreateUnorderedAccessView            = t12CreateUnorderedAccessView;
    t->pfnCreateDepthStencilView               = t12CreateDepthStencilView;
    t->pfnCopyDescriptors                      = t12CopyDescriptors;
    t->pfnCopyDescriptorsSimple                = t12CopyDescriptorsSimple;
    t->pfnCheckResourceVirtualAddress          = t12CheckResourceVirtualAddress;
    t->pfnCalcPrivateDescriptorHeapSize        = t12CalcPrivateDescriptorHeapSize;
    t->pfnCreateDescriptorHeap                 = t12CreateDescriptorHeap;
    t->pfnDestroyDescriptorHeap                = t12DestroyDescriptorHeap;
    t->pfnGetDescriptorSizeInBytes             = t12GetDescriptorSizeInBytes;
    t->pfnGetCPUDescriptorHandleForHeapStart   = t12GetCPUDescriptorHandleForHeapStart;
    t->pfnGetGPUDescriptorHandleForHeapStart   = t12GetGPUDescriptorHandleForHeapStart;
}
