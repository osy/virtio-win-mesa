/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Triton D3D12 DDI: private handle structs behind the D3D12DDI_H*
 * handles.  As in triton.h for D3D11, the runtime allocates
 * the private storage (pfnCalcPrivate*Size), we place our struct in it,
 * and every wrapped Neptune COM pointer is a HOST pointer forwarded
 * verbatim through the wrapper vtbl layer.
 */

#ifndef TRITON12_H
#define TRITON12_H

#include "triton.h"   /* windows.h, d3dkmthk.h, TR_LOG plumbing */

#include <d3d12.h>
#include <d3d12umddi.h>

typedef struct TRITON12_ADAPTER
{
    D3D12DDI_HRTADAPTER     hRTAdapter;
    /* Runtime handles of the filled COMMAND_LIST_3D tables (num=0
     * direct, num=1 bundle), captured in pfnFillDDITable.
     * pfnCreateCommandList must hand the right one back through
     * pfnSetCommandListDDITableCb or the runtime dispatches through a
     * NULL table. */
    D3D12DDI_HRTTABLE       hRTTableCmdList[2];
    /* Host capset TRITON_HOSTCAP_* bits (tritonSharedBridgeAdapterProbe),
     * latched at OpenAdapter12.  Gates every capability whose backing
     * varies by host backend (DXIL/SM6, FL 12_0). */
    UINT32                  HostCaps;
} TRITON12_ADAPTER, *PTRITON12_ADAPTER;

/* Command queues per device tracked for cross-queue ordering.  The cap only
 * bounds the registry walk; overflow degrades to "not ordered against the
 * extras", never to incorrect behaviour. */
#define TRITON12_MAX_QUEUES 16

struct npt_ring;

typedef struct TRITON12_DEVICE
{
    PTRITON12_ADAPTER            pAdapter;
    D3D12DDI_HRTDEVICE           hRTDevice;
    D3DDDI_DEVICECALLBACKS       KTCallbacks;
    CONST D3D12DDI_CORELAYER_DEVICECALLBACKS_0022 *pUMCallbacks;

    /* Neptune COM wrappers (host-side ID3D12Device behind the wrapper
     * vtbl layer).  Populated by tritonCreateDevice12 via
     * npt_d3d12_create_device_internal. */
    ID3D12Device                *pDev;
    /* Host TiledResourcesTier (D3D12_OPTIONS on the inner device),
     * probed once at device create.  >= 1 means the host backs reserved
     * resources sparsely and honours UpdateTileMappings, so
     * t12CreateHeapAndResource forwards CreateReservedResource; 0 takes
     * the committed backing instead. */
    UINT                         HostTiledTier;
    /* One-shot VIOGPU_CTX_INIT on the runtime's kernel device so the
     * KMD can bind exported blobs; see
     * tritonPresentEnsureRuntimeCtx. */
    BOOL                         RuntimeCtxInited;

    /* ---- cross-queue submission ordering (see t12OrderAgainstSiblings) ----
     *
     * Registry of this device's command queues.  Needed because the app's
     * cross-queue GPU waits never reach the host: the D3D12 runtime services
     * fences through dxgkrnl monitored-fence packets and never calls
     * pfnWaitForFence, while t12ExecuteCommandLists forwards the real work to
     * the host over the ring in user mode.  dxgkrnl parks only the kernel
     * packets, so the host sees two independent Vulkan queues with no
     * ordering at all.
     *
     * Guarded by QueueLock, which is only ever taken around the (tiny)
     * registry walk -- never while calling into the inner device. */
    CRITICAL_SECTION             QueueLock;
    BOOL                         QueueLockInit;
    struct TRITON12_QUEUE       *Queues[TRITON12_MAX_QUEUES];
    UINT                         QueueCount;

    /* ---- tile-mapping decode edge (see t12UpdateTileMappings) ----
     *
     * Where in which ring the most recent forwarded tile-mapping update
     * sits.  Work on any OTHER ring that could consume the mapping waits
     * for the host to have decoded past this point first.  Also guarded by
     * QueueLock; cleared when the owning queue goes away. */
    struct npt_ring             *TileMapRing;
    UINT32                       TileMapSeqno;
} TRITON12_DEVICE, *PTRITON12_DEVICE;

static inline PTRITON12_DEVICE
triton12Device(D3D12DDI_HDEVICE hDevice)
{
    return (PTRITON12_DEVICE)hDevice.pDrvPrivate;
}

typedef struct TRITON12_DESCRIPTOR_HEAP
{
    ID3D12DescriptorHeap        *pHeap;
} TRITON12_DESCRIPTOR_HEAP, *PTRITON12_DESCRIPTOR_HEAP;

typedef struct TRITON12_HEAP
{
    ID3D12Heap                  *pHeap;      /* standalone heap, or NULL */
    ID3D12Resource              *pResource;  /* committed create: the resource
                                              * owning the implicit heap, so
                                              * pfnMapHeap can Map it */
    /* Standalone-heap create desc (type/flags/size), kept so a later
     * placed-resource create in this heap can rebuild inner-API
     * arguments without a wire GetDesc round-trip. */
    D3D12_HEAP_DESC              Desc;
} TRITON12_HEAP, *PTRITON12_HEAP;

typedef struct TRITON12_RESOURCE
{
    ID3D12Resource              *pResource;
    /* Create-time DDI desc, kept for CheckSubresourceInfo /
     * footprint math without a wire GetDesc round-trip. */
    D3D12DDIARG_CREATERESOURCE_0003 Desc;
    /* Whole-heap implicit buffer only (API CreateHeap arrives as heap +
     * covering BUFFER): the inner heap placed resources alias into.
     * Borrowed from the owning TRITON12_HEAP (which releases it); placed
     * creates reach it via ReuseBufferGPUVA.BaseAddress.UMD.hResource. */
    ID3D12Heap                  *pPlaceHeap;
    /* Runtime resource handle + the shared-blob KM allocation the
     * runtime's kernel present flips.  Presentable resources only. */
    D3D12DDI_HRTRESOURCE         hRTResource;
    D3DKMT_HANDLE                hKMAllocation;
    /* Cached host GPU VA (pfnCheckResourceVirtualAddress). */
    UINT64                       GpuVa;
    /* Reserved (tiled) resource created on the host with
     * CreateReservedResource: the tile-mapping and CopyTiles DDIs are
     * forwarded for it.  FALSE means committed backing, where the
     * mapping DDIs have nothing to do. */
    BOOL                         TiledHost;
} TRITON12_RESOURCE, *PTRITON12_RESOURCE;

typedef struct TRITON12_QUERYHEAP
{
    ID3D12QueryHeap             *pHeap;
} TRITON12_QUERYHEAP, *PTRITON12_QUERYHEAP;

typedef struct TRITON12_FENCE
{
    ID3D12Fence                 *pFence;
} TRITON12_FENCE, *PTRITON12_FENCE;

typedef struct TRITON12_QUEUE
{
    ID3D12CommandQueue          *pQueue;
    PTRITON12_DEVICE             pDev;
    /* Per-queue kernel context (pfnCreateContextVirtualCb): the runtime
     * binds it to hRTCommandQueue and performs its kernel-side fence
     * signals/waits against it -- without one, Queue::Signal throws
     * STATUS_INVALID_PARAMETER. */
    D3D12DDI_HRTCOMMANDQUEUE     hRTCommandQueue;
    HANDLE                       hKMContext;
    /* Monitored-fence gate state: the runtime services app fences through
     * dxgkrnl packets on hKMContext, which complete when the context's
     * prior DMA completes.  Per ECL batch we Signal pDrainFence on the
     * inner host queue and submit a VIOGPU_CMD_GATE DMA packet
     * that the KMD parks until the host GPU truly reached DrainValue --
     * making every fence the app observes GPU-true.  See tritonQueue12.c. */
    ID3D12Fence                 *pDrainFence;
    UINT64                       DrainValue;
    /* Highest drain value whose Signal is already encoded into the wire
     * ring.  Cross-queue ordering must wait only on values a sibling has
     * actually sent: DrainValue is incremented BEFORE its Signal is
     * encoded, so ordering against it can place a Wait ahead of the
     * matching Signal in ring order, which the host's in-order submission
     * never resolves. */
    UINT64                       DrainVisible;
    /* Auto-reset event the present-fence arm in t12Present waits on, so the
     * frame is not reported to the compositor until the GPU has finished it.
     * One per queue rather than one per frame -- each arm mints its own
     * single-use proxy token, so reusing the handle is safe, and a
     * per-present CreateEvent would churn a handle on every frame. */
    HANDLE                       hPresentArmEvent;
    /* High-resolution 1 ms slice timer for the present wait.  The drain
     * fence's completed value is published through its feedback slot on a
     * ~100 us host cadence -- a faster path than the KMD completion event
     * (SEOC -> host eventfd -> sync worker -> virtio -> DPC -> waiter pool,
     * a multiple-ms floor on the D3D11 side) -- so t12Present polls the
     * value on 1 ms slices with the event as only a wake hint.  A
     * HIGH_RESOLUTION waitable timer, not a Wait timeout: timeouts quantize
     * to the process timer resolution (15.6 ms unless the app raised it). */
    HANDLE                       hPresentPaceTimer;
    /* Slot in pDev->Queues, and the highest drain value this queue has already
     * ordered itself behind for each sibling slot.  Keeping the high-water
     * mark stops us re-emitting a Wait for work we are already ordered after,
     * which would otherwise add one wait per sibling per ECL for the whole
     * run. */
    UINT                         Slot;
    UINT64                       XQueueSeen[TRITON12_MAX_QUEUES];
} TRITON12_QUEUE, *PTRITON12_QUEUE;

/* Publish DrainVisible after a drain Signal has been issued.  Monotonic:
 * concurrent gates on one queue can mint values in one order and return in
 * another. */
static __inline void
t12PublishDrainVisible(PTRITON12_QUEUE q, UINT64 v)
{
    UINT64 cur = q->DrainVisible;
    while (v > cur) {
        UINT64 got = (UINT64)InterlockedCompareExchange64(
            (volatile LONG64 *)&q->DrainVisible, (LONG64)v, (LONG64)cur);
        if (got == cur)
            break;
        cur = got;
    }
}

typedef struct TRITON12_ALLOCATOR
{
    ID3D12CommandAllocator      *pAlloc;
} TRITON12_ALLOCATOR, *PTRITON12_ALLOCATOR;

typedef struct TRITON12_LIST
{
    ID3D12GraphicsCommandList   *pList;
    PTRITON12_DEVICE             pDev;
    /* Recording-thread stamp, kept only under
     * NPT_DEBUG=d3d12_list_migration.  A change between Reset and Close
     * means the runtime migrated this list across threads mid-recording
     * — under multi-ring the wire then splits the list across TLS rings
     * with no intra-list ordering. */
    DWORD                        RecordTid;
} TRITON12_LIST, *PTRITON12_LIST;

/* Each object family installs its own slots into the tables the runtime
 * hands to pfnFillDDITable. */
void triton12InstallDescriptorFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallResourceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallQueueDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallQueueFuncs(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t);
void triton12InstallListDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallListFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *t);
void triton12InstallPipelineFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallPresentFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *t);
void triton12InstallPresentDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);
void triton12InstallQueryDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t);

/* Give a presentable committed resource its shared-blob KM allocation,
 * which the runtime's kernel present needs to flip it. */
BOOL triton12RegisterSharedBlob(PTRITON12_DEVICE p, PTRITON12_RESOURCE r,
                                BOOL primary);

/* ---------- tiled resources: standard tile geometry ---------- */

/* One D3D12 tile of a 2D resource.  An "element" is a texel for an
 * uncompressed format and a 4x4 block for BC, which is the unit the tile
 * shape is defined in. */
typedef struct TRITON12_TILE_SHAPE
{
    UINT ElemBytes;      /* bytes per element */
    UINT BlockEdge;      /* texels per element edge: 1, or 4 for BC */
    UINT ElemW, ElemH;   /* tile extent in elements */
    UINT TexelW, TexelH; /* the same extent in texels */
    UINT RowBytes;       /* one element row of the tile */
} TRITON12_TILE_SHAPE;

static inline void
t12TileShape(DXGI_FORMAT fmt, TRITON12_TILE_SHAPE *out)
{
    UINT elemBytes, blockEdge = 1;
    switch (fmt) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        elemBytes = 8; blockEdge = 4; break;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        elemBytes = 16; blockEdge = 4; break;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
        elemBytes = 1; break;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        elemBytes = 2; break;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        elemBytes = 16; break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        elemBytes = 8; break;
    default:
        /* The broad 32-bit class (R8G8B8A8*, B8G8R8A8*, R10G10B10A2*,
         * R11G11B10, R16G16*, R32*, D32_FLOAT, D24S8, ...).  The 96-bit
         * formats land here too and are wrong for them, but D3D12 has no
         * standard tile shape for 96 bpp and forbids tiling them. */
        elemBytes = 4; break;
    }

    /* A tile holds TILE_SIZE/ElemBytes elements arranged as the squarest
     * power-of-two rectangle, the wider side first.  That reproduces
     * D3D12's standard tile-shape table: 256x256 elements at one byte
     * each, halving alternately down to 64x64 at sixteen. */
    UINT log2Elems = 0;
    while (((UINT)D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES >> log2Elems) >
           elemBytes)
        log2Elems++;

    out->ElemBytes = elemBytes;
    out->BlockEdge = blockEdge;
    out->ElemW = 1u << ((log2Elems + 1u) / 2u);
    out->ElemH = 1u << (log2Elems / 2u);
    out->TexelW = out->ElemW * blockEdge;
    out->TexelH = out->ElemH * blockEdge;
    out->RowBytes = out->ElemW * elemBytes;
}

#endif /* TRITON12_H */
