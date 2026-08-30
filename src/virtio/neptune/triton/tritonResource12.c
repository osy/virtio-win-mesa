/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 DDI heaps + resources.  pfnCreateHeapAndResource triple-
 * dispatches to the inner device (CreateHeap / CreateCommittedResource /
 * CreatePlacedResource); residency is a no-op because the memory is UMA
 * and host-managed, and KM allocations exist only for presentable and
 * shared resources, via the shared-bridge blob export.
 *
 * pfnCalcPrivate{,Opened}HeapAndResourceSizes return a 16-byte struct by
 * value, so their ABI passes a hidden return-buffer pointer ahead of the
 * declared arguments.  They must never be left to a generic stub.
 */

#include "triton12.h"
#include "triton_log.h"

/* ---------- DDI -> API translation helpers ---------- */

static void
t12HeapProps(const D3D12DDIARG_CREATEHEAP_0001 *pH,
             D3D12_HEAP_PROPERTIES *pProps)
{
    memset(pProps, 0, sizeof(*pProps));
    pProps->Type = D3D12_HEAP_TYPE_CUSTOM;
    /* API enums are the DDI enums shifted by one (API adds UNKNOWN=0). */
    pProps->CPUPageProperty = (D3D12_CPU_PAGE_PROPERTY)(pH->CPUPageProperty + 1);
    pProps->MemoryPoolPreference = (D3D12_MEMORY_POOL)(pH->MemoryPool + 1);
    pProps->CreationNodeMask = 0;
    pProps->VisibleNodeMask = 0;
}

static D3D12_HEAP_FLAGS
t12StandaloneHeapFlags(const D3D12DDIARG_CREATEHEAP_0001 *pH)
{
    /* The DDI expresses ALLOW sets; the API expresses DENY sets. */
    D3D12_HEAP_FLAGS f = D3D12_HEAP_FLAG_NONE;
    const BOOL bufs    = !!(pH->Flags & D3D12DDI_HEAP_FLAG_BUFFERS);
    const BOOL rtds    = !!(pH->Flags & D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES);
    const BOOL nonrtds = !!(pH->Flags & D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES);
    if (bufs || rtds || nonrtds) {
        if (!bufs)    f |= D3D12_HEAP_FLAG_DENY_BUFFERS;
        if (!rtds)    f |= D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
        if (!nonrtds) f |= D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
    }
    if (pH->Flags & D3D12DDI_HEAP_FLAG_PRIMARY)
        f |= D3D12_HEAP_FLAG_ALLOW_DISPLAY;
    return f;
}

static void
t12ResourceDesc(const D3D12DDIARG_CREATERESOURCE_0003 *pR,
                D3D12_RESOURCE_DESC *pDesc)
{
    memset(pDesc, 0, sizeof(*pDesc));
    /* D3D12DDI_RT_* and D3D12_RESOURCE_DIMENSION_* agree (1..4). */
    pDesc->Dimension = (D3D12_RESOURCE_DIMENSION)pR->ResourceType;
    pDesc->Alignment = 0;
    pDesc->Width = pR->Width;
    pDesc->Height = pR->Height ? pR->Height : 1;
    pDesc->DepthOrArraySize = pR->DepthOrArraySize ? pR->DepthOrArraySize : 1;
    pDesc->MipLevels = pR->MipLevels;
    pDesc->Format = pR->Format;
    pDesc->SampleDesc = pR->SampleDesc;
    if (!pDesc->SampleDesc.Count)
        pDesc->SampleDesc.Count = 1;

    if (pR->ResourceType == D3D12DDI_RT_BUFFER) {
        pDesc->Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        pDesc->Height = 1;
        pDesc->DepthOrArraySize = 1;
        pDesc->MipLevels = 1;
        pDesc->Format = DXGI_FORMAT_UNKNOWN;
    } else if (pR->Layout == D3D12DDI_TL_ROW_MAJOR) {
        pDesc->Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    } else {
        /* UNDEFINED, and BOTH 64KB tiled layouts: the inner device sees
         * a plain optimal-layout texture.  A reserved resource is either
         * re-created with the tiled layout by the reserved path in
         * t12CreateHeapAndResource (host sparse backing) or fully backed
         * by the committed shim; a placed/committed resource that merely
         * asked for a 64 KB layout has no swizzle to honour here. */
        pDesc->Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }

    D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE;
    if (pR->Flags & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET)
        f |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (pR->Flags & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL) {
        f |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!(pR->Flags & D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE))
            f |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    if (pR->Flags & D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS)
        f |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (pR->Flags & D3D12DDI_RESOURCE_FLAG_0003_CROSS_ADAPTER)
        f |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    if (pR->Flags & D3D12DDI_RESOURCE_FLAG_0003_SIMULTANEOUS_ACCESS)
        f |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    pDesc->Flags = f;
}

/* ---------- sizes (STRUCT RETURN -- never stub) ---------- */

static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY
t12CalcPrivateHeapAndResourceSizes(D3D12DDI_HDEVICE hDevice,
                                   const D3D12DDIARG_CREATEHEAP_0001 *pHeap,
                                   const D3D12DDIARG_CREATERESOURCE_0003 *pRes)
{
    D3D12DDI_HEAP_AND_RESOURCE_SIZES s;
    (void)hDevice;
    s.Heap     = pHeap ? sizeof(TRITON12_HEAP)     : 0;
    s.Resource = pRes  ? sizeof(TRITON12_RESOURCE) : 0;
    /* Committed creates (both descs) still get heap private storage for
     * the implicit heap -- pfnMapHeap arrives on that handle. */
    if (!pHeap && pRes)
        s.Heap = 0;
    return s;
}

static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY
t12CalcPrivateOpenedHeapAndResourceSizes(D3D12DDI_HDEVICE hDevice,
                                         const D3D12DDIARG_OPENHEAP_0003 *pOpen)
{
    D3D12DDI_HEAP_AND_RESOURCE_SIZES s;
    (void)hDevice; (void)pOpen;
    s.Heap     = sizeof(TRITON12_HEAP);
    s.Resource = sizeof(TRITON12_RESOURCE);
    return s;
}

/* ---------- create / destroy ---------- */

/* A single-slice, single-mip TEXTURE2D: one plane of pixels with nothing
 * else to address.  Both the ROW_MAJOR layout rule and the host's shared-
 * surface path are limited to this shape. */
static BOOL
t12IsSimpleTexture2D(const D3D12DDIARG_CREATERESOURCE_0003 *pRes)
{
    return pRes->ResourceType == D3D12DDI_RT_TEXTURE2D &&
           pRes->MipLevels <= 1 &&
           pRes->DepthOrArraySize <= 1;
}

/* The only shape D3D12 permits a ROW_MAJOR -- and therefore CPU-accessible
 * -- texture to have.  Anything else (mipped, arrayed, volume, multisampled,
 * or a render/depth target) can only be laid out opaquely, so answering
 * ROW_MAJOR for it would describe a resource that cannot exist. */
static BOOL
t12RowMajorCapable(const D3D12DDIARG_CREATERESOURCE_0003 *pRes)
{
    return t12IsSimpleTexture2D(pRes) &&
           pRes->SampleDesc.Count <= 1 &&
           !(pRes->Flags & (D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET |
                            D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL));
}

/* D3D12 requires a texture on a CPU-accessible heap to be ROW_MAJOR: only a
 * linear layout has a meaningful CPU address.  The DDI does not say so -- the
 * runtime passes D3D12DDI_TL_UNDEFINED and expects the driver to know -- and
 * t12ResourceDesc maps UNDEFINED to D3D12_TEXTURE_LAYOUT_UNKNOWN, which builds
 * a host resource with an opaque swizzle and no mappable pointer.  pfnMapHeap
 * then reaches ID3D12Resource::Map, which cannot invent a pointer to a
 * non-ROW_MAJOR texture.  The backend does not validate the layout/heap
 * combination, so the create succeeds and only the map fails.
 *
 * A texture PLACED on a CPU-visible app heap is governed by the same rule;
 * that path is left alone because it would need the heap's properties fetched
 * over the wire per create. */
static void
t12ForceRowMajorOnCpuHeap(const D3D12DDIARG_CREATERESOURCE_0003 *pRes,
                          const D3D12_HEAP_PROPERTIES *pProps,
                          D3D12_RESOURCE_DESC *pDesc)
{
    if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
        !t12RowMajorCapable(pRes))
        return;
    /* t12HeapProps always builds a CUSTOM heap, mapping the DDI's
     * CPUPageProperty by +1, so NOT_AVAILABLE is the only non-visible value
     * it can produce. */
    const BOOL cpuVisible =
        pProps->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
        pProps->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    if (cpuVisible)
        pDesc->Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
}

/* Create the present-copy companion for a native simple-2D render target:
 * a second committed resource with the same desc but the shared/display/
 * linear-export heap flags (so the host substitutes its linear shm surface
 * for it), whose blob then backs the KM allocation registered against the
 * GAME resource's runtime handle.  The companion starts and stays in
 * COMMON; its only writer is the CopyResource in t12PresentCompanionCopy,
 * which relies on implicit COMMON-state promotion (verified host-side by
 * d3dmetal-native's present-copy-test). */
static BOOL
t12CreateCompanion(PTRITON12_DEVICE p, PTRITON12_RESOURCE r,
                   const D3D12_HEAP_PROPERTIES *props,
                   const D3D12_RESOURCE_DESC *desc)
{
    const D3D12_HEAP_FLAGS hf =
        D3D12_HEAP_FLAG_ALLOW_DISPLAY | D3D12_HEAP_FLAG_SHARED |
        (D3D12_HEAP_FLAGS)0x40000000u;
    D3D12_RESOURCE_DESC cd = *desc;
    HRESULT hr = ID3D12Device_CreateCommittedResource(
        p->pDev, props, hf, &cd, D3D12_RESOURCE_STATE_COMMON, NULL,
        &IID_ID3D12Resource, (void **)&r->pCompanion);
    if (FAILED(hr) || !r->pCompanion) {
        TR_LOG("12.CreateCompanion: create FAILED 0x%08lx %llux%u fmt=%d",
               (unsigned long)hr, (unsigned long long)desc->Width,
               desc->Height, (int)desc->Format);
        r->pCompanion = NULL;
        return FALSE;
    }
    if (!triton12RegisterSharedBlob(p, r, FALSE, r->pCompanion)) {
        ID3D12Resource_Release(r->pCompanion);
        r->pCompanion = NULL;
        return FALSE;
    }
    TR_LOG("12.CreateCompanion: %llux%u fmt=%d alloc=0x%x",
           (unsigned long long)desc->Width, desc->Height, (int)desc->Format,
           r->hKMAllocation);
    return TRUE;
}

static HRESULT APIENTRY
t12CreateHeapAndResourceCore(D3D12DDI_HDEVICE hDevice,
                         const D3D12DDIARG_CREATEHEAP_0001 *pHeapDesc,
                         D3D12DDI_HHEAP hHeap,
                         D3D12DDI_HRTRESOURCE hRTResource,
                         const D3D12DDIARG_CREATERESOURCE_0003 *pResDesc,
                         const D3D12DDI_CLEAR_VALUES *pClearValues,
                         D3D12DDI_HRESOURCE hResource)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_HEAP h = (PTRITON12_HEAP)hHeap.pDrvPrivate;
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    if (!p || !p->pDev)
        return E_INVALIDARG;
    if (h)
        memset(h, 0, sizeof(*h));
    if (r) {
        memset(r, 0, sizeof(*r));
        if (pResDesc)
            r->Desc = *pResDesc;
        r->hRTResource = hRTResource;
    }

    /* Layout-compatible: DXGI_FORMAT + union{FLOAT[4], {FLOAT,UINT8}}. */
    const D3D12_CLEAR_VALUE *pClear = (const D3D12_CLEAR_VALUE *)pClearValues;

    if (pHeapDesc && pResDesc) {
        /* Committed resource: implicit heap + resource in one call. */
        D3D12_HEAP_PROPERTIES props;
        D3D12_RESOURCE_DESC desc;
        t12HeapProps(pHeapDesc, &props);
        t12ResourceDesc(pResDesc, &desc);
        t12ForceRowMajorOnCpuHeap(pResDesc, &props, &desc);
        /* The recorded desc must state the layout actually used: the
         * allocation-info and subresource-info DDIs answer from it, and the
         * runtime refuses CPU access to a resource it believes is swizzled. */
        if (r && desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
            pResDesc->ResourceType != D3D12DDI_RT_BUFFER)
            r->Desc.Layout = D3D12DDI_TL_ROW_MAJOR;
        const BOOL primary = !!(pHeapDesc->Flags & D3D12DDI_HEAP_FLAG_PRIMARY);
        /* PRIMARY or RT texture.  COHERENT_SYSTEMWIDE (0x8) is NOT a
         * sharing flag: it marks CPU-coherent (UPLOAD/READBACK) memory
         * and rides along on every upload-heap committed create
         * (heapFlags 0xc), so treating it as shared would put SHARED|
         * ALLOW_DISPLAY on plain upload buffers, whose exports then
         * fail.  The rtTex term is required: windowed swapchain back
         * buffers carry no distinguishing DDI flag, and DXGI's
         * CreateSwapChainForHwnd needs their KM allocation to exist or
         * every swapchain create fails with 0x80070057.
         *
         * rtTex matches only what the share path can represent: the host
         * shares through IOSurface, which is one linear plane -- no volume,
         * no array slices, no mip chain.  A resource outside that shape
         * cannot be substituted (a view naming slice or level > 0 has
         * nothing to address), and sharing it also stamps SHARED|
         * ALLOW_DISPLAY and the host's EXPORT_LINEAR_DMABUF onto a resource
         * that wants optimal tiling.  Swapchain back buffers, the only thing
         * this term must catch, always have the simple shape, and
         * t12CheckResourceAllocationHandle still registers a blob lazily for
         * anything that needs one later. */
        const BOOL rtTex =
            t12IsSimpleTexture2D(pResDesc) &&
            !!(pResDesc->Flags & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET);
        const BOOL shared = primary || rtTex;
        /* Companion mode: an rtTex resource is created NATIVE -- optimally
         * tiled, never substituted with a linear shm impostor -- and the
         * KM allocation the swapchain machinery needs is carried by a
         * same-desc SHARED companion surface created beside it
         * (t12CreateCompanion).  t12Present copies the frame into the
         * companion, so what DXGI/DWM consume is unchanged while the
         * game's ~hundreds of render targets per level load stay native.
         * Primaries (fullscreen scanout) keep the direct-shared shape: the
         * scanout consumes their allocation every frame, and their create
         * already means "this IS the displayed surface". */
        const BOOL companion = rtTex && !primary;
        D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
        if (shared && !companion) {
            /* Presentable: SHARED so the host can CreateSharedHandle the
             * resource for the blob export, plus the host's private
             * EXPORT_LINEAR_DMABUF flag -- without it the native shared
             * descriptor build E_NOTIMPLs on OPTIMAL tiling. */
            hf = D3D12_HEAP_FLAG_ALLOW_DISPLAY | D3D12_HEAP_FLAG_SHARED |
                 (D3D12_HEAP_FLAGS)0x40000000u;
        }

        /* ByteSize is not trustworthy in either direction: the runtime's
         * implicit-heap creates leave it ~0, and real API CreateHeap does
         * not reliably carry the size either.  Passing it through builds
         * a UINT64_MAX host heap and kills the render worker.  The
         * covering buffer's Width is the one reliable size, since API
         * CreateHeap always arrives as a BUFFER of exactly the heap
         * size.  Route every
         * buffer create through heap+placed with a Width-derived size;
         * a committed buffer this way is just a dedicated heap, which
         * is what committed means. */
        const UINT64 hbytes = (desc.Width + 0xFFFFull) & ~0xFFFFull;
        const BOOL heapLike = desc.Width && desc.Width <= (64ull << 30);
        if (pResDesc->ResourceType == D3D12DDI_RT_BUFFER && !shared &&
            heapLike && r && h) {
            /* BUFFER + heap desc: this is how API CreateHeap arrives --
             * a covering whole-heap BUFFER rides along regardless of the
             * heap's ALLOW class, texture heaps included.  Build a real
             * inner heap + a placed buffer at 0 so later placed creates
             * (which reference THIS buffer via ReuseBufferGPUVA) can
             * alias into the same inner heap.  Committed buffers take the
             * same shape; a dedicated heap is what committed means.
             * The inner heap drops the tier-1 ALLOW segregation (host is
             * heap tier 2): placed textures must land in heaps whose
             * covering buffer lives there too. */
            D3D12_HEAP_DESC hd;
            memset(&hd, 0, sizeof(hd));
            hd.SizeInBytes = hbytes;
            hd.Properties = props;
            /* Alignment: only the two API-legal values pass through. */
            hd.Alignment = (pHeapDesc->Alignment == 0x10000ull ||
                            pHeapDesc->Alignment == 0x400000ull)
                               ? pHeapDesc->Alignment : 0;
            hd.Flags = D3D12_HEAP_FLAG_NONE;
            HRESULT hhr = ID3D12Device_CreateHeap(
                p->pDev, &hd, &IID_ID3D12Heap, (void **)&h->pHeap);
            if (SUCCEEDED(hhr)) {
                hhr = ID3D12Device_CreatePlacedResource(
                    p->pDev, h->pHeap, 0, &desc,
                    (D3D12_RESOURCE_STATES)pResDesc->InitialResourceState,
                    pClear, &IID_ID3D12Resource, (void **)&r->pResource);
                if (SUCCEEDED(hhr)) {
                    h->pResource = r->pResource;
                    h->Desc = hd;
                    r->pPlaceHeap = h->pHeap;
                    TR_LOG("12.CreateHeapAndResource(heap+buf): heap=%p size=%llu "
                           "heapFlags=0x%x state=0x%x -> 0x%08lx", (void *)h,
                           (unsigned long long)hd.SizeInBytes,
                           (unsigned)pHeapDesc->Flags,
                           (unsigned)pResDesc->InitialResourceState,
                           (unsigned long)hhr);
                    return hhr;
                }
                ID3D12Heap_Release(h->pHeap);
                h->pHeap = NULL;
            }
            TR_LOG("12.CreateHeapAndResource(heap+buf): inner heap path "
                   "failed 0x%08lx; falling back to committed",
                   (unsigned long)hhr);
        }

        HRESULT hr = ID3D12Device_CreateCommittedResource(
            p->pDev, &props, hf, &desc,
            (D3D12_RESOURCE_STATES)pResDesc->InitialResourceState,
            pClear, &IID_ID3D12Resource, (void **)&r->pResource);
        TR_LOG("12.CreateHeapAndResource(committed): type=%d w=%llu fmt=%d "
               "state=0x%x heapFlags=0x%x heapBytes=0x%llx heapAlign=0x%llx "
               "resFlags=0x%x primary=%d shared=%d -> 0x%08lx",
               (int)pResDesc->ResourceType,
               (unsigned long long)pResDesc->Width, (int)pResDesc->Format,
               (unsigned)pResDesc->InitialResourceState,
               (unsigned)pHeapDesc->Flags,
               (unsigned long long)pHeapDesc->ByteSize,
               (unsigned long long)pHeapDesc->Alignment,
               (unsigned)pResDesc->Flags,
               primary, shared, (unsigned long)hr);
        if (SUCCEEDED(hr) && h) {
            /* pfnMapHeap arrives on the implicit heap's handle. */
            h->pResource = r->pResource;
        }
        if (SUCCEEDED(hr) && shared) {
            if (companion) {
                if (!t12CreateCompanion(p, r, &props, &desc)) {
                    /* Companion build failed: fall back to an eager
                     * direct share so the swapchain still works -- the
                     * resource becomes an impostor, which renders. */
                    TR_LOG("12.CreateHeapAndResource: companion FAILED for "
                           "%llux%u fmt=%d; falling back to direct share",
                           (unsigned long long)desc.Width, desc.Height,
                           (int)desc.Format);
                    ID3D12Resource_Release(r->pResource);
                    r->pResource = NULL;
                    hf = D3D12_HEAP_FLAG_ALLOW_DISPLAY |
                         D3D12_HEAP_FLAG_SHARED |
                         (D3D12_HEAP_FLAGS)0x40000000u;
                    hr = ID3D12Device_CreateCommittedResource(
                        p->pDev, &props, hf, &desc,
                        (D3D12_RESOURCE_STATES)pResDesc->InitialResourceState,
                        pClear, &IID_ID3D12Resource, (void **)&r->pResource);
                    if (h)
                        h->pResource = r->pResource;
                    if (SUCCEEDED(hr))
                        triton12RegisterSharedBlob(p, r, primary, NULL);
                }
            } else {
                triton12RegisterSharedBlob(p, r, primary, NULL);
            }
        }
        return hr;
    }

    if (pHeapDesc) {
        /* Standalone heap. */
        D3D12_HEAP_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.SizeInBytes = pHeapDesc->ByteSize;
        t12HeapProps(pHeapDesc, &desc.Properties);
        desc.Alignment = pHeapDesc->Alignment;
        desc.Flags = t12StandaloneHeapFlags(pHeapDesc);
        HRESULT hr = ID3D12Device_CreateHeap(
            p->pDev, &desc, &IID_ID3D12Heap, (void **)&h->pHeap);
        if (SUCCEEDED(hr))
            h->Desc = desc;
        TR_LOG("12.CreateHeapAndResource(heap): heap=%p size=%llu flags=0x%x -> 0x%08lx",
               (void *)h, (unsigned long long)pHeapDesc->ByteSize,
               (unsigned)pHeapDesc->Flags, (unsigned long)hr);
        return hr;
    }

    if (pResDesc) {
        /* Placed resource.  The runtime passes no heap handle here
         * (hHeap is NULL); the placement arrives as
         * ReuseBufferGPUVA.BaseAddress.UMD = {the heap's covering
         * whole-heap BUFFER resource, byte offset}.  Resolve that buffer
         * to its inner heap and place for real, so aliasing semantics
         * and VA = heap base + offset both come from the host. */
        if (!r)
            return E_INVALIDARG;
        PTRITON12_RESOURCE base = (PTRITON12_RESOURCE)
            pResDesc->ReuseBufferGPUVA.BaseAddress.UMD.hResource.pDrvPrivate;
        const UINT64 off =
            pResDesc->ReuseBufferGPUVA.BaseAddress.UMD.Offset;
        /* A 64 KB tiled layout does not by itself mean a reserved
         * resource: a PLACED resource may ask for
         * 64KB_UNDEFINED_SWIZZLE too -- aliasing transient render
         * targets in one heap is the common case -- and it arrives with
         * the same NULL heap desc.  What separates them is the
         * placement base: a reserved resource has no heap behind it
         * (hResource NULL) and gets its memory only through
         * UpdateTileMappings; a placed one is backed by its heap and is
         * never mapped, so treating it as reserved leaves it unmapped
         * and reading zero. */
        const BOOL tiled =
            (pResDesc->Layout == D3D12DDI_TL_64KB_TILE_UNDEFINED_SWIZZLE ||
             pResDesc->Layout == D3D12DDI_TL_64KB_TILE_STANDARD_SWIZZLE) &&
            base == NULL;
        D3D12_RESOURCE_DESC desc;
        t12ResourceDesc(pResDesc, &desc);
        HRESULT hr;
        if (tiled && p->HostTiledTier >= 1) {
            /* Reserved (tiled) resource, host has sparse backing: forward
             * the reserved create as-is.  Only the tiles the app maps
             * (t12UpdateTileMappings -> host) get memory; unmapped tiles
             * read zero.  Host RAM is then the app's tile-pool budget
             * instead of the textures' virtual size. */
            D3D12_RESOURCE_DESC rdesc = desc;
            rdesc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
            hr = ID3D12Device_CreateReservedResource(
                p->pDev, &rdesc,
                (D3D12_RESOURCE_STATES)pResDesc->InitialResourceState,
                pClear, &IID_ID3D12Resource, (void **)&r->pResource);
            if (SUCCEEDED(hr) && r->pResource) {
                r->TiledHost = TRUE;
                TR_LOG("12.CreateHeapAndResource(reserved): res=%p type=%d "
                       "w=%llu h=%u fmt=%d mips=%u arr=%u state=0x%x -> 0x%08lx",
                       (void *)r, (int)pResDesc->ResourceType,
                       (unsigned long long)pResDesc->Width,
                       (unsigned)pResDesc->Height, (int)pResDesc->Format,
                       (unsigned)pResDesc->MipLevels,
                       (unsigned)pResDesc->DepthOrArraySize,
                       (unsigned)pResDesc->InitialResourceState,
                       (unsigned long)hr);
                return hr;
            }
            TR_LOG("12.CreateHeapAndResource(reserved): host create failed "
                   "0x%08lx; committed-backing fallback", (unsigned long)hr);
            r->pResource = NULL;
        }
        if (tiled) {
            /* Reserved (tiled) resource: committed-backing shim.  The
             * whole resource is backed by a zero-initialized committed
             * allocation, so tier-2 semantics hold trivially
             * ("everything is mapped", unmapped reads never happen);
             * UpdateTileMappings/CopyTileMappings are queue-side no-ops
             * and CopyTiles becomes region copies.  Cost: full physical
             * backing up front -- the opposite of the feature's purpose,
             * and the only shape a host without sparse backing supports. */
            D3D12_HEAP_PROPERTIES props;
            memset(&props, 0, sizeof(props));
            props.Type = D3D12_HEAP_TYPE_DEFAULT;
            hr = ID3D12Device_CreateCommittedResource(
                p->pDev, &props, D3D12_HEAP_FLAG_NONE, &desc,
                (D3D12_RESOURCE_STATES)pResDesc->InitialResourceState,
                pClear, &IID_ID3D12Resource, (void **)&r->pResource);
            TR_LOG("12.CreateHeapAndResource(reserved->committed): res=%p type=%d "
                   "w=%llu h=%u fmt=%d mips=%u state=0x%x -> 0x%08lx",
                   (void *)r, (int)pResDesc->ResourceType,
                   (unsigned long long)pResDesc->Width,
                   (unsigned)pResDesc->Height, (int)pResDesc->Format,
                   (unsigned)pResDesc->MipLevels,
                   (unsigned)pResDesc->InitialResourceState,
                   (unsigned long)hr);
            return hr;
        }
        if (base && base->pPlaceHeap) {
            hr = ID3D12Device_CreatePlacedResource(
                p->pDev, base->pPlaceHeap, off, &desc,
                (D3D12_RESOURCE_STATES)pResDesc->InitialResourceState,
                pClear, &IID_ID3D12Resource, (void **)&r->pResource);
            TR_LOG("12.CreateHeapAndResource(placed): type=%d w=%llu fmt=%d "
                   "off=%llu state=0x%x -> 0x%08lx",
                   (int)pResDesc->ResourceType,
                   (unsigned long long)pResDesc->Width,
                   (int)pResDesc->Format, (unsigned long long)off,
                   (unsigned)pResDesc->InitialResourceState,
                   (unsigned long)hr);
            return hr;
        }
        /* No placeable backing (heap-only create, or an unexpected
         * runtime shape): committed fallback.  NEVER E_NOTIMPL here --
         * a failed create makes the runtime REMOVE THE DEVICE
         * with 0x887A0020. */
        D3D12_HEAP_PROPERTIES props;
        memset(&props, 0, sizeof(props));
        props.Type = D3D12_HEAP_TYPE_DEFAULT;
        hr = ID3D12Device_CreateCommittedResource(
            p->pDev, &props, D3D12_HEAP_FLAG_NONE, &desc,
            (D3D12_RESOURCE_STATES)pResDesc->InitialResourceState,
            pClear, &IID_ID3D12Resource, (void **)&r->pResource);
        TR_LOG("12.CreateHeapAndResource(placed): NO BACKING (base=%p "
               "heap=%p off=%llu) -- committed fallback, aliasing LOST "
               "-> 0x%08lx",
               (void *)base, base ? (void *)base->pPlaceHeap : NULL,
               (unsigned long long)off, (unsigned long)hr);
        return hr;
    }

    return E_INVALIDARG;
}

/* Heaps and committed resources need a kernel allocation whether or not
 * they are presentable: ID3D12Device1::SetResidencyPriority is serviced
 * inside the D3D12 runtime, which returns E_INVALIDARG for an object that
 * owns none without consulting the driver.  Resources created here live on
 * the host, so only the presentable ones get an allocation from the
 * shared-blob export; every other heap-desc create gets the memory-less
 * placeholder.  Placed and reserved resources are left without one: they
 * own no allocation on any driver -- residency belongs to their heap. */
static HRESULT APIENTRY
t12CreateHeapAndResource(D3D12DDI_HDEVICE hDevice,
                         const D3D12DDIARG_CREATEHEAP_0001 *pHeapDesc,
                         D3D12DDI_HHEAP hHeap,
                         D3D12DDI_HRTRESOURCE hRTResource,
                         const D3D12DDIARG_CREATERESOURCE_0003 *pResDesc,
                         const D3D12DDI_CLEAR_VALUES *pClearValues,
                         D3D12DDI_HRESOURCE hResource)
{
    const HRESULT hr = t12CreateHeapAndResourceCore(
        hDevice, pHeapDesc, hHeap, hRTResource, pResDesc, pClearValues,
        hResource);
    if (SUCCEEDED(hr) && pHeapDesc) {
        PTRITON12_DEVICE p = triton12Device(hDevice);
        PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
        PTRITON12_HEAP h = (PTRITON12_HEAP)hHeap.pDrvPrivate;
        if (p && r && r->pResource) {
            if (!r->hKMAllocation)
                triton12RegisterResidencyAlloc(p, r);
        } else if (p && h && h->pHeap && !h->hKMAllocation) {
            /* Heap-only create: no covering buffer rode along, so the
             * heap carries the placeholder itself. */
            h->hKMAllocation =
                triton12AllocResidencyOnly(p, hRTResource.handle);
        }
    }
    return hr;
}

static VOID APIENTRY
t12DestroyHeapAndResource(D3D12DDI_HDEVICE hDevice, D3D12DDI_HHEAP hHeap,
                          D3D12DDI_HRESOURCE hResource)
{
    PTRITON12_HEAP h = (PTRITON12_HEAP)hHeap.pDrvPrivate;
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    (void)hDevice;
    if (r && r->pCompanion) {
        ID3D12Resource_Release(r->pCompanion);
        r->pCompanion = NULL;
    }
    if (r && r->pResource) {
        ID3D12Resource_Release(r->pResource);
        r->pResource = NULL;
    }
    if (h) {
        /* h->pResource is a borrow of r->pResource (committed) -- the
         * release above covers it. */
        if (h->pHeap) {
            ID3D12Heap_Release(h->pHeap);
            h->pHeap = NULL;
        }
        h->pResource = NULL;
    }
}

/* ---------- tiled resources: mip packing ---------- */

/* Extent of mip `level`, floored at one texel. */
static UINT64
t12MipExtent(UINT64 base, UINT level)
{
    const UINT64 v = base >> level;
    return v ? v : 1;
}

/* pfnGetMipPacking: which trailing mips of a tiled resource share one
 * tile run, and how many tiles that run costs.  A mip at least one tile
 * wide and one tile tall is "standard" and owns whole tiles; everything
 * below that is packed together, because a tile is the smallest unit
 * either side can map.  Reporting the tail as packed lets the runtime
 * address it as one region -- per-mip addressing of a sub-tile mip is
 * not something the host's sparse translation can honour, and it charges
 * a whole tile per mip for a tail that fits in one.
 *
 * D3D12 defines packed mips per array slice, so the count is not scaled
 * by the array size; a volume texture's packed run does repeat per depth
 * slice. */
VOID APIENTRY
triton12GetMipPacking(D3D12DDI_HDEVICE hDevice, D3D12DDI_HRESOURCE hRes,
                      UINT *pNumPackedMips, UINT *pNumTilesForPackedMips)
{
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hRes.pDrvPrivate;
    (void)hDevice;
    UINT packed = 0, tiles = 0;
    if (r && r->Desc.ResourceType != D3D12DDI_RT_BUFFER) {
        TRITON12_TILE_SHAPE tile;
        t12TileShape((DXGI_FORMAT)r->Desc.Format, &tile);
        const UINT mips = r->Desc.MipLevels ? r->Desc.MipLevels : 1;

        UINT standard = 0;
        while (standard < mips &&
               t12MipExtent(r->Desc.Width, standard) >= tile.TexelW &&
               t12MipExtent(r->Desc.Height, standard) >= tile.TexelH)
            standard++;
        packed = mips - standard;

        if (packed) {
            UINT64 bytes = 0;
            for (UINT m = standard; m < mips; m++) {
                const UINT64 w = t12MipExtent(r->Desc.Width, m);
                const UINT64 h = t12MipExtent(r->Desc.Height, m);
                bytes += ((w + tile.BlockEdge - 1) / tile.BlockEdge) *
                         ((h + tile.BlockEdge - 1) / tile.BlockEdge) *
                         tile.ElemBytes;
            }
            const UINT64 tileBytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
            tiles = (UINT)((bytes + tileBytes - 1) / tileBytes);
            if (!tiles)
                tiles = 1;
            if (r->Desc.ResourceType == D3D12DDI_RT_TEXTURE3D)
                tiles *= r->Desc.DepthOrArraySize ? r->Desc.DepthOrArraySize : 1;
        }
    }
    if (pNumPackedMips) *pNumPackedMips = packed;
    if (pNumTilesForPackedMips) *pNumTilesForPackedMips = tiles;
    TR_LOG_HOT("12.GetMipPacking: res=%p mips=%u -> packed=%u tiles=%u",
               (void *)r, r ? (unsigned)r->Desc.MipLevels : 0u, packed, tiles);
}

/* ---------- map ---------- */

static HRESULT APIENTRY
t12MapHeap(D3D12DDI_HDEVICE hDevice, D3D12DDI_HHEAP hHeap, VOID **ppData)
{
    PTRITON12_HEAP h = (PTRITON12_HEAP)hHeap.pDrvPrivate;
    (void)hDevice;
    if (!ppData)
        return E_INVALIDARG;
    *ppData = NULL;
    if (h && h->pResource) {
        /* Committed resource's implicit heap: map the whole resource
         * (engages the shmem zero-wire Map path of npt_d3d12_heap.c). */
        return ID3D12Resource_Map(h->pResource, 0, NULL, ppData);
    }
    TR_LOG("12.MapHeap: standalone heap -- NOTIMPL");
    return E_NOTIMPL;
}

static VOID APIENTRY
t12UnmapHeap(D3D12DDI_HDEVICE hDevice, D3D12DDI_HHEAP hHeap)
{
    PTRITON12_HEAP h = (PTRITON12_HEAP)hHeap.pDrvPrivate;
    (void)hDevice;
    if (h && h->pResource)
        ID3D12Resource_Unmap(h->pResource, 0, NULL);
}

/* ---------- allocation info ---------- */

static VOID APIENTRY
t12CheckResourceAllocationInfo(D3D12DDI_HDEVICE hDevice,
                               const D3D12DDIARG_CREATERESOURCE_0003 *pRes,
                               D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS OptFlags,
                               UINT32 AlignmentRestriction,
                               UINT VisibleNodeMask,
                               D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *pOut)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    (void)OptFlags; (void)VisibleNodeMask;
    if (!pOut)
        return;
    memset(pOut, 0, sizeof(*pOut));
    /* Alignment fields must never be zero-filled (the 1074/1012 caps
     * trap all over again): the runtime pre-fills the info with an
     * invalid sentinel and keeps it when any field fails validation. */
    pOut->ResourceDataAlignment = 65536;
    pOut->AdditionalDataHeaderAlignment = 65536;
    pOut->AdditionalDataAlignment = 65536;
    if (!p || !p->pDev || !pRes)
        return;

    D3D12_RESOURCE_DESC desc;
    t12ResourceDesc(pRes, &desc);
    D3D12_RESOURCE_ALLOCATION_INFO info;
    p->pDev->lpVtbl->GetResourceAllocationInfo(p->pDev, &info, 0, 1, &desc);

    pOut->ResourceDataSize = info.SizeInBytes;
    UINT64 align = info.Alignment;
    if (AlignmentRestriction && AlignmentRestriction > align)
        align = AlignmentRestriction;
    pOut->ResourceDataAlignment = (UINT32)align;
    /* The answer must be the CONCRETE layout the driver will use --
     * echoing UNDEFINED for an opaque texture makes the runtime treat
     * the info as invalid: it computes the resource size as ~0 aligned
     * down (18446744073709486080 in the debug-layer message) and every
     * placed-texture create then fails E_INVALIDARG. */
    if (pRes->ResourceType == D3D12DDI_RT_BUFFER)
        pOut->Layout = D3D12DDI_TL_ROW_MAJOR;
    else if (pRes->Layout == D3D12DDI_TL_UNDEFINED)
        /* ROW_MAJOR for a CPU-mappable shape, an opaque 64KB swizzle for
         * everything else.  The runtime takes this answer as the concrete
         * layout and refuses CPU access to a resource it believes is
         * swizzled -- Map fails E_OUTOFMEMORY inside the runtime, without
         * ever reaching this driver, which breaks upload by Map +
         * WriteToSubresource, the documented UMA path.  D3D12 requires
         * ROW_MAJOR for a texture on a CPU-accessible heap, and that is
         * what t12ForceRowMajorOnCpuHeap builds, so the two agree.  The
         * GPU-side layout of a DEFAULT-heap texture is unaffected: it is
         * chosen at create time and its strides are reported truthfully by
         * t12CheckSubresourceInfo. */
        pOut->Layout = t12RowMajorCapable(pRes)
                           ? D3D12DDI_TL_ROW_MAJOR
                           : D3D12DDI_TL_64KB_TILE_UNDEFINED_SWIZZLE;
    else
        pOut->Layout = pRes->Layout;
}

static VOID APIENTRY
t12CheckExistingResourceAllocationInfo(D3D12DDI_HDEVICE hDevice,
                                       D3D12DDI_HRESOURCE hResource,
                                       D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 *pOut)
{
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    if (!pOut)
        return;
    memset(pOut, 0, sizeof(*pOut));
    if (r)
        t12CheckResourceAllocationInfo(hDevice, &r->Desc,
                                       D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,
                                       0, 0, pOut);
}

static VOID APIENTRY
t12CheckSubresourceInfo(D3D12DDI_HDEVICE hDevice, D3D12DDI_HRESOURCE hResource,
                        UINT Subresource, D3D12DDI_SUBRESOURCE_INFO *pOut)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    if (!pOut)
        return;
    memset(pOut, 0, sizeof(*pOut));
    if (!p || !p->pDev || !r)
        return;

    D3D12_RESOURCE_DESC desc;
    t12ResourceDesc(&r->Desc, &desc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed;
    UINT numRows = 0;
    UINT64 rowSize = 0, total = 0;
    memset(&placed, 0, sizeof(placed));
    p->pDev->lpVtbl->GetCopyableFootprints(p->pDev, &desc, Subresource, 1, 0,
                                           &placed, &numRows, &rowSize, &total);
    pOut->Offset = placed.Offset;
    /* For a row-major resource report the TIGHT row size, not the
     * pitch-aligned footprint.  The backend stores such a texture with its
     * rows packed, while GetCopyableFootprints answers with D3D12's
     * 256-aligned pitch; handing the runtime the aligned value makes it
     * write each row at a stride the texture is not read at, which shifts
     * every row after the first by (pitch - width) bytes.  The two values
     * are equal whenever the tight size is already aligned, so only
     * unaligned widths are affected. */
    if (desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && rowSize) {
        pOut->RowStride = rowSize;
        pOut->DepthStride = (UINT64)rowSize * numRows;
    } else {
        pOut->RowStride = placed.Footprint.RowPitch;
        pOut->DepthStride = (UINT64)placed.Footprint.RowPitch * numRows;
    }
}

/* ---------- residency: UMA no-ops ----------
 *
 * Deliberately not forwarded to the inner device.  The host memory is UMA and
 * host-managed, so residency is the host's to decide; the backend answers
 * MakeResident "Unsupported" and evicting frees nothing.  Reporting
 * synchronous success is both correct and cheaper. */

static HRESULT APIENTRY
t12MakeResident(D3D12DDI_HDEVICE hDevice, D3D12DDIARG_MAKERESIDENT_0001 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return S_OK; /* synchronous success: paging fence outputs unused */
}

static HRESULT APIENTRY
t12Evict(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_EVICT *pArgs)
{
    (void)hDevice; (void)pArgs;
    return S_OK;
}

static HRESULT APIENTRY
t12OfferResources(D3D12DDI_HDEVICE hDevice, const D3D12DDIARG_OFFERRESOURCES *pArgs)
{
    (void)hDevice; (void)pArgs;
    return S_OK;
}

static HRESULT APIENTRY
t12ReclaimResources(D3D12DDI_HDEVICE hDevice,
                    D3D12DDIARG_RECLAIMRESOURCES_0001 *pArgs)
{
    (void)hDevice;
    if (pArgs && pArgs->pDiscarded) {
        for (UINT i = 0; i < pArgs->NumObjects; i++)
            pArgs->pDiscarded[i] = FALSE; /* content preserved */
    }
    return S_OK;
}

static HRESULT APIENTRY
t12OpenHeapAndResource(D3D12DDI_HDEVICE hDevice,
                       const D3D12DDIARG_OPENHEAP_0003 *pOpen,
                       D3D12DDI_HHEAP hHeap, D3D12DDI_HRTRESOURCE hRTResource,
                       D3D12DDI_HRESOURCE hResource)
{
    (void)hDevice; (void)pOpen; (void)hHeap; (void)hRTResource; (void)hResource;
    TR_LOG("12.OpenHeapAndResource -- NOTIMPL");
    return E_NOTIMPL;
}

/* ---------- fences ---------- */

static SIZE_T APIENTRY
t12CalcPrivateFenceSize(D3D12DDI_HDEVICE hDevice,
                        const D3D12DDIARG_CREATE_FENCE *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_FENCE);
}

static HRESULT APIENTRY
t12CreateFence(D3D12DDI_HDEVICE hDevice, D3D12DDI_HFENCE hFence,
               const D3D12DDIARG_CREATE_FENCE *pArgs)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_FENCE f = (PTRITON12_FENCE)hFence.pDrvPrivate;
    if (!p || !p->pDev || !f || !pArgs)
        return E_INVALIDARG;
    memset(f, 0, sizeof(*f));
    HRESULT hr = ID3D12Device_CreateFence(
        p->pDev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
        (void **)&f->pFence);
    TR_LOG("12.CreateFence: count=%u -> 0x%08lx",
           pArgs->FenceCount, (unsigned long)hr);
    return hr;
}

static VOID APIENTRY
t12DestroyFence(D3D12DDI_HDEVICE hDevice, D3D12DDI_HFENCE hFence)
{
    PTRITON12_FENCE f = (PTRITON12_FENCE)hFence.pDrvPrivate;
    (void)hDevice;
    if (f && f->pFence) {
        ID3D12Fence_Release(f->pFence);
        f->pFence = NULL;
    }
}

void
triton12InstallResourceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnMapHeap                              = t12MapHeap;
    t->pfnUnmapHeap                            = t12UnmapHeap;
    t->pfnCalcPrivateHeapAndResourceSizes      = t12CalcPrivateHeapAndResourceSizes;
    t->pfnCreateHeapAndResource                = t12CreateHeapAndResource;
    t->pfnDestroyHeapAndResource               = t12DestroyHeapAndResource;
    t->pfnMakeResident                         = t12MakeResident;
    t->pfnEvict                                = t12Evict;
    t->pfnCalcPrivateOpenedHeapAndResourceSizes = t12CalcPrivateOpenedHeapAndResourceSizes;
    t->pfnOpenHeapAndResource                  = t12OpenHeapAndResource;
    t->pfnCheckResourceAllocationInfo          = t12CheckResourceAllocationInfo;
    t->pfnCheckSubresourceInfo                 = t12CheckSubresourceInfo;
    t->pfnCheckExistingResourceAllocationInfo  = t12CheckExistingResourceAllocationInfo;
    t->pfnOfferResources                       = t12OfferResources;
    t->pfnReclaimResources                     = t12ReclaimResources;
    t->pfnCalcPrivateFenceSize                 = t12CalcPrivateFenceSize;
    t->pfnCreateFence                          = t12CreateFence;
    t->pfnDestroyFence                         = t12DestroyFence;
}
