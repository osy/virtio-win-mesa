/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 DDI command allocators + command lists: mostly 1:1 forwards to
 * the inner ID3D12GraphicsCommandList wrapper with mechanical struct
 * translation.
 *
 * pfnCreateCommandList MUST hand the runtime a command-list DDI table
 * via pfnSetCommandListDDITableCb, using the D3D12DDI_HRTTABLE captured
 * in pfnFillDDITable; without it the runtime's first dispatch jumps
 * through a NULL table.
 */

#include "triton12.h"
#include "triton_log.h"

#include "npt_env.h"

static void t12ListStampTid(PTRITON12_LIST l);

static D3D12_COMMAND_LIST_TYPE
t12ListApiType(D3D12DDI_COMMAND_LIST_TYPE Type,
               D3D12DDI_COMMAND_QUEUE_FLAGS QueueFlags)
{
    if (Type == D3D12DDI_COMMAND_LIST_TYPE_BUNDLE)
        return D3D12_COMMAND_LIST_TYPE_BUNDLE;
    /* 3D FIRST: direct lists carry 3D|COMPUTE|COPY (see
     * t12QueueFlagsToApiType -- compute-first hung the GPU). */
    if (QueueFlags & D3D12DDI_COMMAND_QUEUE_FLAG_3D)
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (QueueFlags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE)
        return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (QueueFlags & D3D12DDI_COMMAND_QUEUE_FLAG_COPY)
        return D3D12_COMMAND_LIST_TYPE_COPY;
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

/* ---------- command allocator ---------- */

static SIZE_T APIENTRY
t12CalcPrivateCommandAllocatorSize(D3D12DDI_HDEVICE hDevice,
                                   const D3D12DDIARG_CREATECOMMANDALLOCATOR *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_ALLOCATOR);
}

static HRESULT APIENTRY
t12CreateCommandAllocator(D3D12DDI_HDEVICE hDevice,
                          const D3D12DDIARG_CREATECOMMANDALLOCATOR *pArgs)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_ALLOCATOR a =
        pArgs ? (PTRITON12_ALLOCATOR)pArgs->hDrvCommandAllocator.pDrvPrivate : NULL;
    if (!p || !p->pDev || !a)
        return E_INVALIDARG;
    memset(a, 0, sizeof(*a));
    HRESULT hr = ID3D12Device_CreateCommandAllocator(
        p->pDev, t12ListApiType(pArgs->Type, pArgs->QueueFlags),
        &IID_ID3D12CommandAllocator, (void **)&a->pAlloc);
    TR_LOG("12.CreateCommandAllocator: type=%d qflags=0x%x -> 0x%08lx",
           (int)pArgs->Type, (unsigned)pArgs->QueueFlags, (unsigned long)hr);
    return hr;
}

static VOID APIENTRY
t12DestroyCommandAllocator(D3D12DDI_HDEVICE hDevice,
                           D3D12DDI_HCOMMANDALLOCATOR hAllocator)
{
    PTRITON12_ALLOCATOR a = (PTRITON12_ALLOCATOR)hAllocator.pDrvPrivate;
    (void)hDevice;
    if (a && a->pAlloc) {
        ID3D12CommandAllocator_Release(a->pAlloc);
        a->pAlloc = NULL;
    }
}

/* Unlike its Destroy sibling this slot takes no device handle. */
static VOID APIENTRY
t12ResetCommandAllocator(D3D12DDI_HCOMMANDALLOCATOR hAllocator)
{
    PTRITON12_ALLOCATOR a = (PTRITON12_ALLOCATOR)hAllocator.pDrvPrivate;
    if (a && a->pAlloc)
        ID3D12CommandAllocator_Reset(a->pAlloc);
}

/* ---------- command pool / recorder (_0040) ---------- */

static UINT
t12PoolIndex(D3D12_COMMAND_LIST_TYPE t)
{
    return ((UINT)t < TRITON12_POOL_TYPES) ? (UINT)t : 0u;
}

static SIZE_T APIENTRY
t12CalcPrivateCommandPoolSize(D3D12DDI_HDEVICE hDevice,
                              const D3D12DDIARG_CREATE_COMMAND_POOL_0040 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_POOL);
}

static HRESULT APIENTRY
t12CreateCommandPool(D3D12DDI_HDEVICE hDevice,
                     const D3D12DDIARG_CREATE_COMMAND_POOL_0040 *pArgs,
                     D3D12DDI_HCOMMANDPOOL_0040 hPool)
{
    PTRITON12_POOL pool = (PTRITON12_POOL)hPool.pDrvPrivate;
    (void)hDevice; (void)pArgs;
    if (!pool)
        return E_INVALIDARG;
    memset(pool, 0, sizeof(*pool));
    return S_OK;
}

static VOID APIENTRY
t12DestroyCommandPool(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDPOOL_0040 hPool)
{
    PTRITON12_POOL pool = (PTRITON12_POOL)hPool.pDrvPrivate;
    (void)hDevice;
    if (!pool)
        return;
    for (UINT i = 0; i < TRITON12_POOL_TYPES; i++) {
        if (pool->pAlloc[i]) {
            ID3D12CommandAllocator_Release(pool->pAlloc[i]);
            pool->pAlloc[i] = NULL;
        }
    }
}

static VOID APIENTRY
t12ResetCommandPool(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDPOOL_0040 hPool)
{
    PTRITON12_POOL pool = (PTRITON12_POOL)hPool.pDrvPrivate;
    (void)hDevice;
    if (!pool)
        return;
    for (UINT i = 0; i < TRITON12_POOL_TYPES; i++)
        if (pool->pAlloc[i])
            ID3D12CommandAllocator_Reset(pool->pAlloc[i]);
}

static ID3D12CommandAllocator *
t12PoolAllocator(PTRITON12_DEVICE p, PTRITON12_POOL pool,
                 D3D12_COMMAND_LIST_TYPE type)
{
    const UINT i = t12PoolIndex(type);
    if (!pool->pAlloc[i]) {
        HRESULT hr = ID3D12Device_CreateCommandAllocator(
            p->pDev, type, &IID_ID3D12CommandAllocator,
            (void **)&pool->pAlloc[i]);
        TR_LOG("12.CommandPool: allocator for type %d -> 0x%08lx", (int)type,
               (unsigned long)hr);
        if (FAILED(hr))
            pool->pAlloc[i] = NULL;
    }
    return pool->pAlloc[i];
}

static SIZE_T APIENTRY
t12CalcPrivateCommandRecorderSize(D3D12DDI_HDEVICE hDevice,
                                  const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_RECORDER);
}

static HRESULT APIENTRY
t12CreateCommandRecorder(D3D12DDI_HDEVICE hDevice,
                         const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040 *pArgs,
                         D3D12DDI_HCOMMANDRECORDER_0040 hRecorder)
{
    PTRITON12_RECORDER rec = (PTRITON12_RECORDER)hRecorder.pDrvPrivate;
    (void)hDevice;
    if (!rec || !pArgs)
        return E_INVALIDARG;
    memset(rec, 0, sizeof(*rec));
    rec->QueueFlags = pArgs->QueueFlags;
    return S_OK;
}

static VOID APIENTRY
t12DestroyCommandRecorder(D3D12DDI_HDEVICE hDevice,
                          D3D12DDI_HCOMMANDRECORDER_0040 hRecorder)
{
    (void)hDevice; (void)hRecorder;
}

static VOID APIENTRY
t12CommandRecorderSetCommandPoolAsTarget(D3D12DDI_HDEVICE hDevice,
                                         D3D12DDI_HCOMMANDRECORDER_0040 hRecorder,
                                         D3D12DDI_HCOMMANDPOOL_0040 hPool)
{
    PTRITON12_RECORDER rec = (PTRITON12_RECORDER)hRecorder.pDrvPrivate;
    (void)hDevice;
    if (rec)
        rec->pPool = (PTRITON12_POOL)hPool.pDrvPrivate;
}

/* ---------- command list ---------- */

static SIZE_T APIENTRY
t12CalcPrivateCommandListSize(D3D12DDI_HDEVICE hDevice,
                              const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_LIST);
}

static HRESULT APIENTRY
t12CreateCommandList(D3D12DDI_HDEVICE hDevice,
                     const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *pArgs)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_LIST l =
        pArgs ? (PTRITON12_LIST)pArgs->hDrvCommandList.pDrvPrivate : NULL;
    PTRITON12_ALLOCATOR a =
        pArgs ? (PTRITON12_ALLOCATOR)pArgs->hDrvCommandAllocator.pDrvPrivate : NULL;
    if (!p || !p->pDev || !l || !a || !a->pAlloc)
        return E_INVALIDARG;
    memset(l, 0, sizeof(*l));
    l->pDev = p;

    HRESULT hr = ID3D12Device_CreateCommandList(
        p->pDev, 0, t12ListApiType(pArgs->Type, pArgs->QueueFlags), a->pAlloc,
        NULL, &IID_ID3D12GraphicsCommandList, (void **)&l->pList);
    TR_LOG("12.CreateCommandList: type=%d qflags=0x%x -> 0x%08lx",
           (int)pArgs->Type, (unsigned)pArgs->QueueFlags, (unsigned long)hr);
    if (FAILED(hr))
        return hr;

    /* Hand the runtime this list's dispatch table. */
    if (p->pUMCallbacks && p->pUMCallbacks->pfnSetCommandListDDITableCb &&
        p->pAdapter) {
        UINT idx = (pArgs->Type == D3D12DDI_COMMAND_LIST_TYPE_BUNDLE) ? 1 : 0;
        p->pUMCallbacks->pfnSetCommandListDDITableCb(
            pArgs->hRTCommandList, p->pAdapter->hRTTableCmdList[idx]);
    }
    return S_OK;
}

static SIZE_T APIENTRY
t12CalcPrivateCommandListSize0040(D3D12DDI_HDEVICE hDevice,
                                  const D3D12DDIARG_CREATE_COMMAND_LIST_0040 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_LIST);
}

/* No allocator rides the _0040 create: the host list is created against
 * the device's scratch allocator for its type and closed at once, and the
 * runtime's paired ResetCommandList (same ID) rebinds it to the recorder's
 * pool before anything is recorded. */
static HRESULT APIENTRY
t12CreateCommandList0040(D3D12DDI_HDEVICE hDevice,
                         const D3D12DDIARG_CREATE_COMMAND_LIST_0040 *pArgs,
                         D3D12DDI_HCOMMANDLIST hList,
                         D3D12DDI_HRTCOMMANDLIST hRTList)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    if (!p || !p->pDev || !l || !pArgs)
        return E_INVALIDARG;
    memset(l, 0, sizeof(*l));
    l->pDev = p;
    const D3D12_COMMAND_LIST_TYPE type =
        t12ListApiType(pArgs->Type, pArgs->QueueFlags);
    l->ApiType = (UINT)type;

    /* The host list must be created against SOME allocator, and an
     * allocator may back only one RECORDING list at a time -- create
     * leaves the list recording until the Close below.  Two threads
     * creating lists of the same type would therefore collide on a
     * shared scratch allocator and the second create would fail, which
     * surfaces much later as recording sent to an object the host never
     * registered.  Hold the device lock across create+Close so the
     * scratch allocator backs one recording list at a time. */
    const UINT si = t12PoolIndex(type);
    HRESULT hr;
    EnterCriticalSection(&p->QueueLock);
    if (!p->pScratchAlloc[si]) {
        HRESULT ahr = ID3D12Device_CreateCommandAllocator(
            p->pDev, type, &IID_ID3D12CommandAllocator,
            (void **)&p->pScratchAlloc[si]);
        if (FAILED(ahr))
            p->pScratchAlloc[si] = NULL;
    }
    if (!p->pScratchAlloc[si]) {
        LeaveCriticalSection(&p->QueueLock);
        return E_OUTOFMEMORY;
    }
    hr = ID3D12Device_CreateCommandList(
        p->pDev, 0, type, p->pScratchAlloc[si], NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&l->pList);
    if (SUCCEEDED(hr))
        ID3D12GraphicsCommandList_Close(l->pList);
    LeaveCriticalSection(&p->QueueLock);

    TR_LOG("12.CreateCommandList(0040): type=%d qflags=0x%x id=%llu -> 0x%08lx",
           (int)pArgs->Type, (unsigned)pArgs->QueueFlags,
           (unsigned long long)pArgs->ID, (unsigned long)hr);
    if (FAILED(hr))
        return hr;
    l->AwaitingReset = TRUE;

    if (p->pUMCallbacks && p->pUMCallbacks->pfnSetCommandListDDITableCb &&
        p->pAdapter) {
        UINT idx = (pArgs->Type == D3D12DDI_COMMAND_LIST_TYPE_BUNDLE) ? 1 : 0;
        p->pUMCallbacks->pfnSetCommandListDDITableCb(
            hRTList, p->pAdapter->hRTTableCmdList[idx]);
    }
    return S_OK;
}

static VOID APIENTRY
t12ResetCommandList0040(D3D12DDI_HCOMMANDLIST hList,
                        const D3D12DDIARG_RESETCOMMANDLIST_0040 *pArgs)
{
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    PTRITON12_RECORDER rec =
        pArgs ? (PTRITON12_RECORDER)pArgs->hDrvCommandRecorder.pDrvPrivate : NULL;
    if (!l || !l->pList || !l->pDev || !rec || !rec->pPool) {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            TR_LOG("12.ResetCommandList(0040): recorder %p has no target pool",
                   (void *)rec);
        return;
    }
    ID3D12CommandAllocator *alloc = t12PoolAllocator(
        l->pDev, rec->pPool, (D3D12_COMMAND_LIST_TYPE)l->ApiType);
    if (!alloc)
        return;
    l->RecordTid = GetCurrentThreadId();
    l->AwaitingReset = FALSE;
    HRESULT hr = ID3D12GraphicsCommandList_Reset(l->pList, alloc, NULL);
    TR_LOG_HOT("12.ResetCommandList(0040) -> 0x%08lx", (unsigned long)hr);
    if (FAILED(hr)) {
        /* pfnResetCommandList is VOID: without pfnSetErrorCb the API-level
         * Reset returns S_OK and the failure resurfaces only at Close. */
        TR_LOG("12.ResetCommandList(0040) FAILED hr=0x%08lx list=%p",
               (unsigned long)hr, (void *)l);
        if (l->pDev->pUMCallbacks && l->pDev->pUMCallbacks->pfnSetErrorCb)
            l->pDev->pUMCallbacks->pfnSetErrorCb(l->pDev->hRTDevice, hr);
    }
}

static VOID APIENTRY
t12DestroyCommandList(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDLIST hList)
{
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    (void)hDevice;
    if (l && l->pList) {
        ID3D12GraphicsCommandList_Release(l->pList);
        l->pList = NULL;
    }
}

/* ---------- list table (D3D12DDI_COMMAND_LIST_FUNCS_3D_0022) ---------- */

static VOID APIENTRY
t12CloseCommandList(D3D12DDI_HCOMMANDLIST hList)
{
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    if (!l || !l->pList)
        return;
    if (l->AwaitingReset) {
        /* R4 create-then-close never saw its paired reset: the host list
         * is already closed, so this close is a no-op and anything the
         * runtime recorded in between was lost.  Loud, once. */
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            TR_LOG("12.CloseCommandList: list %p closed without a reset after "
                   "its _0040 create", (void *)l);
        return;
    }
    t12ListStampTid(l);
    HRESULT hr = ID3D12GraphicsCommandList_Close(l->pList);
    TR_LOG_HOT("12.CloseCommandList -> 0x%08lx", (unsigned long)hr);
    if (FAILED(hr)) {
        /* A failed Close surfaces to the app only as device removal at
         * ExecuteCommandLists, and TR_LOG_HOT compiles out by default, so
         * this trace is unconditional. */
        TR_LOG("12.CloseCommandList FAILED hr=0x%08lx list=%p tid=%lu",
               (unsigned long)hr, (void *)l,
               (unsigned long)GetCurrentThreadId());
        if (l->pDev && l->pDev->pUMCallbacks &&
            l->pDev->pUMCallbacks->pfnSetErrorCb)
            l->pDev->pUMCallbacks->pfnSetErrorCb(l->pDev->hRTDevice, hr);
    }
}

static VOID APIENTRY
t12ResetCommandList(D3D12DDI_HCOMMANDLIST hList,
                    const D3D12DDIARG_RESETCOMMANDLIST *pArgs)
{
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    PTRITON12_ALLOCATOR a =
        pArgs ? (PTRITON12_ALLOCATOR)pArgs->hDrvCommandAllocator.pDrvPrivate : NULL;
    if (!l || !l->pList || !a || !a->pAlloc)
        return;
    l->RecordTid = GetCurrentThreadId();
    HRESULT hr = ID3D12GraphicsCommandList_Reset(l->pList, a->pAlloc, NULL);
    TR_LOG_HOT("12.ResetCommandList -> 0x%08lx", (unsigned long)hr);
    if (FAILED(hr)) {
        /* pfnResetCommandList is VOID: without pfnSetErrorCb the API-level
         * Reset returns S_OK and the failure resurfaces only at Close. */
        TR_LOG("12.ResetCommandList FAILED hr=0x%08lx list=%p",
               (unsigned long)hr, (void *)l);
        if (l->pDev && l->pDev->pUMCallbacks &&
            l->pDev->pUMCallbacks->pfnSetErrorCb)
            l->pDev->pUMCallbacks->pfnSetErrorCb(l->pDev->hRTDevice, hr);
    }
}

static VOID APIENTRY
t12CopyBufferRegion(D3D12DDI_HCOMMANDLIST hList,
                    D3D12DDIARG_BUFFER_PLACEMENT Dst,
                    D3D12DDIARG_BUFFER_PLACEMENT Src, UINT64 SrcBytes)
{
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    PTRITON12_RESOURCE dst =
        (PTRITON12_RESOURCE)Dst.BaseAddress.UMD.hResource.pDrvPrivate;
    PTRITON12_RESOURCE src =
        (PTRITON12_RESOURCE)Src.BaseAddress.UMD.hResource.pDrvPrivate;
    if (!l || !l->pList || !dst || !dst->pResource || !src || !src->pResource)
        return;
    ID3D12GraphicsCommandList_CopyBufferRegion(
        l->pList, dst->pResource, Dst.BaseAddress.UMD.Offset,
        src->pResource, Src.BaseAddress.UMD.Offset, SrcBytes);
}

static VOID APIENTRY
t12ResourceBarrier(D3D12DDI_HCOMMANDLIST hList, UINT Count,
                   const D3D12DDIARG_RESOURCE_BARRIER_0022 *pBarriers)
{
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    if (!l || !l->pList || !Count || !pBarriers)
        return;

    D3D12_RESOURCE_BARRIER stackBar[8];
    D3D12_RESOURCE_BARRIER *bar = stackBar;
    if (Count > 8) {
        bar = (D3D12_RESOURCE_BARRIER *)malloc(Count * sizeof(*bar));
        if (!bar)
            return;
    }
    UINT n = 0;
    for (UINT i = 0; i < Count; i++) {
        const D3D12DDIARG_RESOURCE_BARRIER_0022 *b = &pBarriers[i];
        memset(&bar[n], 0, sizeof(bar[n]));
        switch (b->Type) {
        case D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION: {
            PTRITON12_RESOURCE r =
                (PTRITON12_RESOURCE)b->Transition.hResource.pDrvPrivate;
            if (!r || !r->pResource)
                continue;
            bar[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bar[n].Flags = (D3D12_RESOURCE_BARRIER_FLAGS)b->Flags;
            bar[n].Transition.pResource = r->pResource;
            bar[n].Transition.Subresource = b->Transition.Subresource;
            bar[n].Transition.StateBefore =
                (D3D12_RESOURCE_STATES)b->Transition.StateBefore;
            bar[n].Transition.StateAfter =
                (D3D12_RESOURCE_STATES)b->Transition.StateAfter;
            n++;
            break;
        }
        case D3D12DDI_RESOURCE_BARRIER_TYPE_UAV: {
            PTRITON12_RESOURCE r =
                (PTRITON12_RESOURCE)b->UAV.hResource.pDrvPrivate;
            bar[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            bar[n].UAV.pResource = (r && r->pResource) ? r->pResource : NULL;
            n++;
            break;
        }
        case D3D12DDI_RESOURCE_BARRIER_TYPE_ALIASING:
            /* Deprecated two-resource form (pre-0022 runtimes): the 0022
             * union doesn't carry its payload; emit the conservative
             * all-memory aliasing barrier. */
            bar[n].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            bar[n].Aliasing.pResourceBefore = NULL;
            bar[n].Aliasing.pResourceAfter = NULL;
            n++;
            break;
        case D3D12DDI_RESOURCE_BARRIER_TYPE_0022_RANGED: {
            /* App AliasingBarrier arrives as RANGED + FLAG_0022_ALIASING
             * in the 0022 revision.  Dropping these left placed-resource
             * aliasing unordered on the host, so forward them as API
             * aliasing barriers.  NULL before/after = conservative
             * all-aliased-memory barrier; the specific resource is a
             * refinement, correctness only needs the flush. */
            if (b->Flags & D3D12DDI_RESOURCE_BARRIER_FLAG_0022_ALIASING) {
                PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)
                    b->Ranged.hResource.pDrvPrivate;
                bar[n].Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                bar[n].Aliasing.pResourceBefore = NULL;
                bar[n].Aliasing.pResourceAfter =
                    (r && r->pResource) ? r->pResource : NULL;
                n++;
            }
            /* Non-aliasing ranged barriers (atomic-copy ranges) have no
             * API equivalent; submission order covers them. */
            break;
        }
        default:
            break;
        }
    }
    if (n)
        ID3D12GraphicsCommandList_ResourceBarrier(l->pList, n, bar);
    if (bar != stackBar)
        free(bar);
}

/* ---------- command signatures (ExecuteIndirect) ----------
 *
 * A stubbed ExecuteIndirect drops draws silently rather than failing, so
 * a title that issues its bulk geometry this way renders a near-empty
 * scene at inflated frame rates.  The DDI argument-desc layout matches
 * the API struct field-for-field and the two enums agree by value, so
 * the desc array passes through with a cast. */

typedef struct TRITON12_CMDSIG
{
    ID3D12CommandSignature *pSig;
} TRITON12_CMDSIG;

/* Defined later in this file / in tritonPipeline12.c. */
static PTRITON12_LIST t12List(D3D12DDI_HCOMMANDLIST h);
ID3D12RootSignature *triton12RootSig(D3D12DDI_HROOTSIGNATURE h);

static SIZE_T APIENTRY
t12CalcPrivateCommandSignatureSize(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_CMDSIG);
}

static HRESULT APIENTRY
t12CreateCommandSignature(D3D12DDI_HDEVICE hDevice,
                          const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *pArgs,
                          D3D12DDI_HCOMMANDSIGNATURE hSig)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    TRITON12_CMDSIG *s = (TRITON12_CMDSIG *)hSig.pDrvPrivate;
    if (!p || !p->pDev || !s || !pArgs)
        return E_INVALIDARG;
    memset(s, 0, sizeof(*s));

    D3D12_COMMAND_SIGNATURE_DESC d;
    memset(&d, 0, sizeof(d));
    d.ByteStride = pArgs->ByteStride;
    d.NumArgumentDescs = pArgs->NumArgumentDescs;
    d.pArgumentDescs =
        (const D3D12_INDIRECT_ARGUMENT_DESC *)pArgs->pArgumentDescs;
    d.NodeMask = 0;

    ID3D12RootSignature *rs = triton12RootSig(pArgs->hRootSignature);
    HRESULT hr = ID3D12Device_CreateCommandSignature(
        p->pDev, &d, rs, &IID_ID3D12CommandSignature, (void **)&s->pSig);
    TR_LOG("12.CreateCommandSignature: stride=%u nargs=%u rs=%p -> 0x%08lx",
           pArgs->ByteStride, pArgs->NumArgumentDescs, (void *)rs,
           (unsigned long)hr);
    return hr;
}

static VOID APIENTRY
t12DestroyCommandSignature(D3D12DDI_HDEVICE hDevice,
                           D3D12DDI_HCOMMANDSIGNATURE hSig)
{
    TRITON12_CMDSIG *s = (TRITON12_CMDSIG *)hSig.pDrvPrivate;
    (void)hDevice;
    if (s && s->pSig) {
        ID3D12CommandSignature_Release(s->pSig);
        s->pSig = NULL;
    }
}

static VOID APIENTRY
t12OmSetBlendFactor(D3D12DDI_HCOMMANDLIST hList, CONST FLOAT BlendFactor[4])
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList)
        ID3D12GraphicsCommandList_OMSetBlendFactor(l->pList, BlendFactor);
}

static VOID APIENTRY
t12OmSetStencilRef(D3D12DDI_HCOMMANDLIST hList, UINT StencilRef)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList)
        ID3D12GraphicsCommandList_OMSetStencilRef(l->pList, StencilRef);
}

static VOID APIENTRY
t12ClearRootArguments(D3D12DDI_HCOMMANDLIST hList)
{
    /* Runtime housekeeping notification on list reset; the host list is
     * freshly Reset at the same point, so there is nothing to clear. */
    (void)hList;
}

static VOID APIENTRY
t12ExecuteIndirect(D3D12DDI_HCOMMANDLIST hList,
                   D3D12DDI_HCOMMANDSIGNATURE hSig, UINT MaxCommandCount,
                   D3D12DDIARG_BUFFER_PLACEMENT ArgumentBuffer,
                   D3D12DDIARG_BUFFER_PLACEMENT CountBuffer)
{
    PTRITON12_LIST l = t12List(hList);
    TRITON12_CMDSIG *s = (TRITON12_CMDSIG *)hSig.pDrvPrivate;
    PTRITON12_RESOURCE arg = (PTRITON12_RESOURCE)
        ArgumentBuffer.BaseAddress.UMD.hResource.pDrvPrivate;
    PTRITON12_RESOURCE cnt = (PTRITON12_RESOURCE)
        CountBuffer.BaseAddress.UMD.hResource.pDrvPrivate;
    if (!l || !l->pList || !s || !s->pSig || !arg || !arg->pResource)
        return;
    ID3D12GraphicsCommandList_ExecuteIndirect(
        l->pList, s->pSig, MaxCommandCount, arg->pResource,
        ArgumentBuffer.BaseAddress.UMD.Offset,
        (cnt && cnt->pResource) ? cnt->pResource : NULL,
        CountBuffer.BaseAddress.UMD.Offset);
}

void
triton12InstallListDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnCalcPrivateCommandAllocatorSize = t12CalcPrivateCommandAllocatorSize;
    t->pfnCreateCommandAllocator          = t12CreateCommandAllocator;
    t->pfnDestroyCommandAllocator         = t12DestroyCommandAllocator;
    t->pfnResetCommandAllocator           = t12ResetCommandAllocator;
    t->pfnCalcPrivateCommandListSize      = t12CalcPrivateCommandListSize;
    t->pfnCreateCommandList               = t12CreateCommandList;
    t->pfnDestroyCommandList              = t12DestroyCommandList;
    t->pfnCalcPrivateCommandSignatureSize = t12CalcPrivateCommandSignatureSize;
    t->pfnCreateCommandSignature          = t12CreateCommandSignature;
    t->pfnDestroyCommandSignature         = t12DestroyCommandSignature;
}

/* ---------- draw path ---------- */

ID3D12RootSignature *triton12RootSig(D3D12DDI_HROOTSIGNATURE h);
ID3D12PipelineState *triton12Pso(D3D12DDI_HPIPELINESTATE h);

/* Report recording-thread migrations (see TRITON12_LIST.RecordTid).
 * Called from the hot list entries, so the whole check sits behind
 * NPT_DEBUG=d3d12_list_migration. */
static void
t12ListStampTid(PTRITON12_LIST l)
{
    if (!NPT_DEBUG(D3D12_LIST_MIGRATION))
        return;
    DWORD tid = GetCurrentThreadId();
    if (l->RecordTid == 0) {
        l->RecordTid = tid;
        return;
    }
    if (l->RecordTid != tid) {
        static LONG s_migrations;
        LONG n = InterlockedIncrement(&s_migrations);
        if (n <= 16 || (n & 1023) == 0)
            TR_LOG("12.list MIGRATION #%ld: list=%p tid %lu -> %lu",
                   (long)n, (void *)l, (unsigned long)l->RecordTid,
                   (unsigned long)tid);
        l->RecordTid = tid;
    }
}

static PTRITON12_LIST
t12List(D3D12DDI_HCOMMANDLIST h)
{
    return (PTRITON12_LIST)h.pDrvPrivate;
}

static VOID APIENTRY
t12SetPipelineState(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HPIPELINESTATE hPso)
{
    PTRITON12_LIST l = t12List(hList);
    ID3D12PipelineState *pso = triton12Pso(hPso);
    if (l && l->pList && pso)
        ID3D12GraphicsCommandList_SetPipelineState(l->pList, pso);
}

static VOID APIENTRY
t12SetGraphicsRootSignature(D3D12DDI_HCOMMANDLIST hList,
                            D3D12DDI_HROOTSIGNATURE hRS)
{
    PTRITON12_LIST l = t12List(hList);
    ID3D12RootSignature *rs = triton12RootSig(hRS);
    if (l && l->pList && rs)
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(l->pList, rs);
}

static VOID APIENTRY
t12SetComputeRootSignature(D3D12DDI_HCOMMANDLIST hList,
                           D3D12DDI_HROOTSIGNATURE hRS)
{
    PTRITON12_LIST l = t12List(hList);
    ID3D12RootSignature *rs = triton12RootSig(hRS);
    if (l && l->pList && rs)
        ID3D12GraphicsCommandList_SetComputeRootSignature(l->pList, rs);
}

static VOID APIENTRY
t12SetDescriptorHeaps(D3D12DDI_HCOMMANDLIST hList, UINT Num,
                      D3D12DDI_HDESCRIPTORHEAP *pHeaps)
{
    PTRITON12_LIST l = t12List(hList);
    if (!l || !l->pList || !Num || !pHeaps)
        return;
    ID3D12DescriptorHeap *heaps[2];
    UINT n = 0;
    for (UINT i = 0; i < Num && n < 2; i++) {
        PTRITON12_DESCRIPTOR_HEAP h =
            (PTRITON12_DESCRIPTOR_HEAP)pHeaps[i].pDrvPrivate;
        if (h && h->pHeap)
            heaps[n++] = h->pHeap;
    }
    if (n)
        ID3D12GraphicsCommandList_SetDescriptorHeaps(l->pList, n, heaps);
}

static VOID APIENTRY
t12IaSetTopology(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_PRIMITIVE_TOPOLOGY Topo)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList) t12ListStampTid(l);
    if (l && l->pList)
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(
            l->pList, (D3D12_PRIMITIVE_TOPOLOGY)Topo);
}

static VOID APIENTRY
t12RsSetViewports(D3D12DDI_HCOMMANDLIST hList, UINT Count,
                  const D3D12DDI_VIEWPORT *pViewports)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList && Count && pViewports)
        ID3D12GraphicsCommandList_RSSetViewports(
            l->pList, Count, (const D3D12_VIEWPORT *)pViewports);
}

static VOID APIENTRY
t12RsSetScissorRects(D3D12DDI_HCOMMANDLIST hList, UINT Count,
                     const D3D12DDI_RECT *pRects)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList && Count && pRects)
        ID3D12GraphicsCommandList_RSSetScissorRects(
            l->pList, Count, (const D3D12_RECT *)pRects);
}

static VOID APIENTRY
t12OMSetRenderTargets(D3D12DDI_HCOMMANDLIST hList, UINT Num,
                      const D3D12DDI_CPU_DESCRIPTOR_HANDLE *pRTs,
                      BOOL SingleHandle,
                      const D3D12DDI_CPU_DESCRIPTOR_HANDLE *pDSV)
{
    PTRITON12_LIST l = t12List(hList);
    if (!l || !l->pList)
        return;
    /* Handles are host descriptor values computed from our host heap
     * starts + host increment: forward verbatim. */
    D3D12_CPU_DESCRIPTOR_HANDLE rts[8];
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    UINT n = (Num > 8) ? 8 : Num;
    /* SingleHandle: the API reads only rts[0] as a range start. */
    for (UINT i = 0; i < n && pRTs; i++)
        rts[i].ptr = (SIZE_T)pRTs[SingleHandle ? 0 : i].ptr;
    if (pDSV)
        dsv.ptr = (SIZE_T)pDSV->ptr;
    ID3D12GraphicsCommandList_OMSetRenderTargets(
        l->pList, n, (n && pRTs) ? rts : NULL, SingleHandle,
        pDSV ? &dsv : NULL);
}

static VOID APIENTRY
t12ClearRenderTargetView(D3D12DDI_HCOMMANDLIST hList,
                         D3D12DDI_CPU_DESCRIPTOR_HANDLE View,
                         const FLOAT Color[4], UINT NumRects,
                         const D3D12DDI_RECT *pRects)
{
    PTRITON12_LIST l = t12List(hList);
    if (!l || !l->pList)
        return;
    D3D12_CPU_DESCRIPTOR_HANDLE v;
    v.ptr = (SIZE_T)View.ptr;
    ID3D12GraphicsCommandList_ClearRenderTargetView(
        l->pList, v, Color, NumRects, (const D3D12_RECT *)pRects);
}

static VOID APIENTRY
t12DrawInstanced(D3D12DDI_HCOMMANDLIST hList, UINT VertexCountPerInstance,
                 UINT InstanceCount, UINT StartVertexLocation,
                 UINT StartInstanceLocation)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList) {
        ID3D12GraphicsCommandList_DrawInstanced(
            l->pList, VertexCountPerInstance, InstanceCount,
            StartVertexLocation, StartInstanceLocation);
    }
}

static VOID APIENTRY
t12DrawIndexedInstanced(D3D12DDI_HCOMMANDLIST hList, UINT IndexCount,
                        UINT InstanceCount, UINT StartIndex,
                        INT BaseVertex, UINT StartInstance)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList) t12ListStampTid(l);
    if (l && l->pList)
        ID3D12GraphicsCommandList_DrawIndexedInstanced(
            l->pList, IndexCount, InstanceCount, StartIndex, BaseVertex,
            StartInstance);
}

static VOID APIENTRY
t12Dispatch(D3D12DDI_HCOMMANDLIST hList, UINT X, UINT Y, UINT Z)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList) t12ListStampTid(l);
    if (l && l->pList)
        ID3D12GraphicsCommandList_Dispatch(l->pList, X, Y, Z);
}

static void
t12CopyLocation(const D3D12DDIARG_BUFFER_PLACEMENT *pBP,
                D3D12DDIARG_PLACED_RESOURCE Placed,
                D3D12_TEXTURE_COPY_LOCATION *out)
{
    PTRITON12_RESOURCE r =
        (PTRITON12_RESOURCE)pBP->BaseAddress.UMD.hResource.pDrvPrivate;
    memset(out, 0, sizeof(*out));
    out->pResource = (r && r->pResource) ? r->pResource : NULL;
    if (Placed.Layout == D3D12DDI_RL_SELECT_SUBRESOURCE) {
        out->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        out->SubresourceIndex = (UINT)pBP->BaseAddress.UMD.Offset;
    } else {
        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *pl =
            (const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *)Placed.pLayout;
        out->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        out->PlacedFootprint.Offset = pBP->BaseAddress.UMD.Offset;
        if (pl) {
            out->PlacedFootprint.Footprint.Format = pl->Format;
            out->PlacedFootprint.Footprint.Width  = pl->PhysicalWidth;
            out->PlacedFootprint.Footprint.Height = pl->PhysicalHeight;
            out->PlacedFootprint.Footprint.Depth  = pl->PhysicalDepth;
            out->PlacedFootprint.Footprint.RowPitch = pl->Pitch;
        }
    }
}

static VOID APIENTRY
t12CopyTextureRegion(D3D12DDI_HCOMMANDLIST hList,
                     const D3D12DDIARG_BUFFER_PLACEMENT *pDst,
                     D3D12DDIARG_PLACED_RESOURCE DstPlaced,
                     UINT DstX, UINT DstY, UINT DstZ,
                     const D3D12DDIARG_BUFFER_PLACEMENT *pSrc,
                     D3D12DDIARG_PLACED_RESOURCE SrcPlaced,
                     const D3D12DDI_BOX *pSrcBox)
{
    PTRITON12_LIST l = t12List(hList);
    if (!l || !l->pList || !pDst || !pSrc)
        return;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    t12CopyLocation(pDst, DstPlaced, &dst);
    t12CopyLocation(pSrc, SrcPlaced, &src);
    if (!dst.pResource || !src.pResource)
        return;
    ID3D12GraphicsCommandList_CopyTextureRegion(
        l->pList, &dst, DstX, DstY, DstZ, &src, (const D3D12_BOX *)pSrcBox);
}

#define T12_TILE_BYTES ((UINT64)D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES)

/* The DDI's tile-copy flags carry the API's values. */
C_ASSERT((UINT)D3D12DDI_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE ==
         (UINT)D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);

/* ---------- CopyTiles ----------
 *
 * A resource the host backs sparsely takes the inner CopyTiles verbatim.
 * On the committed-backing shim the resource is a fully backed ordinary
 * texture, so the copy is open-coded as region copies: each tile of the
 * buffer maps to one standard-tile-shaped texel block of the
 * subresource, copied by one CopyTextureRegion each (the buffer's
 * per-tile layout is linear within the tile, which matches a footprint
 * of exactly one tile). */

static VOID APIENTRY
t12CopyTiles(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HRESOURCE hRes,
             const D3D12DDI_TILED_RESOURCE_COORDINATE *pStart,
             const D3D12DDI_TILE_REGION_SIZE *pSize,
             D3D12DDI_HRESOURCE hBuffer, UINT64 BufferStartOffsetInBytes,
             D3D12DDI_TILE_COPY_FLAGS Flags)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hRes.pDrvPrivate;
    PTRITON12_RESOURCE b = (PTRITON12_RESOURCE)hBuffer.pDrvPrivate;
    if (!l || !l->pList || !r || !r->pResource || !b || !b->pResource ||
        !pStart || !pSize)
        return;

    if (r->TiledHost) {
        /* Sparsely backed on the host: it owns the tile addressing. */
        ID3D12GraphicsCommandList_CopyTiles(
            l->pList, r->pResource,
            (const D3D12_TILED_RESOURCE_COORDINATE *)pStart,
            (const D3D12_TILE_REGION_SIZE *)pSize, b->pResource,
            BufferStartOffsetInBytes, (D3D12_TILE_COPY_FLAGS)Flags);
        return;
    }

    /* LINEAR_BUFFER_TO_SWIZZLED = buffer -> resource; otherwise (incl. no
     * flags, whose buffer-side "swizzled" order equals linear for a
     * committed resource) resource -> buffer. */
    const BOOL toResource =
        !!(Flags & D3D12DDI_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);

    if (r->Desc.ResourceType == D3D12DDI_RT_BUFFER) {
        /* Reserved buffer: tiles are plain spans of tile-size bytes. */
        UINT64 resOff = (UINT64)pStart->X * T12_TILE_BYTES;
        UINT64 bytes = (UINT64)pSize->NumTiles * T12_TILE_BYTES;
        if (toResource)
            ID3D12GraphicsCommandList_CopyBufferRegion(
                l->pList, r->pResource, resOff, b->pResource,
                BufferStartOffsetInBytes, bytes);
        else
            ID3D12GraphicsCommandList_CopyBufferRegion(
                l->pList, b->pResource, BufferStartOffsetInBytes,
                r->pResource, resOff, bytes);
        return;
    }

    TRITON12_TILE_SHAPE tile;
    t12TileShape(r->Desc.Format, &tile);
    const UINT tw = tile.TexelW, th = tile.TexelH;
    const UINT mips = r->Desc.MipLevels ? r->Desc.MipLevels : 1;
    const UINT mip = pStart->Subresource % mips;
    const UINT mipW = (UINT)(r->Desc.Width >> mip) ? (UINT)(r->Desc.Width >> mip) : 1;
    const UINT mipH = (r->Desc.Height >> mip) ? (r->Desc.Height >> mip) : 1;
    const UINT gridW = (mipW + tw - 1) / tw; /* tiles per row */
    const UINT gridH = (mipH + th - 1) / th;

    /* Walk the region tile by tile: boxes x->y->z, linear runs row-major
     * from the start coordinate within one subresource. */
    UINT n = pSize->NumTiles;
    if (n > 16384) {
        TR_LOG("12.CopyTiles: clamping %u tiles to 16384", n);
        n = 16384;
    }
    UINT bw = pSize->UseBox ? pSize->Width : 0;
    UINT tx = pStart->X, ty = pStart->Y;
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        TR_LOG("12.CopyTiles: %s %u tiles (box=%d) fmt=%d tile=%ux%u "
               "grid=%ux%u", toResource ? "buf->res" : "res->buf", n,
               (int)pSize->UseBox, (int)r->Desc.Format, tw, th, gridW, gridH);
    if (pStart->Z || (pSize->UseBox && pSize->Depth > 1)) {
        TR_LOG("12.CopyTiles: 3D tiled regions unsupported (z=%u depth=%u)",
               pStart->Z, pSize->UseBox ? pSize->Depth : 0);
        return;
    }
    for (UINT i = 0; i < n; i++) {
        if (ty >= gridH) {
            TR_LOG("12.CopyTiles: region leaves subresource (tile %u)", i);
            break;
        }
        const UINT x0 = tx * tw, y0 = ty * th;
        const UINT cw = (x0 + tw <= mipW) ? tw : mipW - x0;
        const UINT ch = (y0 + th <= mipH) ? th : mipH - y0;

        D3D12_TEXTURE_COPY_LOCATION tex, buf;
        memset(&tex, 0, sizeof(tex));
        memset(&buf, 0, sizeof(buf));
        tex.pResource = r->pResource;
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        tex.SubresourceIndex = pStart->Subresource;
        buf.pResource = b->pResource;
        buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        buf.PlacedFootprint.Offset =
            BufferStartOffsetInBytes + (UINT64)i * T12_TILE_BYTES;
        buf.PlacedFootprint.Footprint.Format = r->Desc.Format;
        buf.PlacedFootprint.Footprint.Width = cw;
        buf.PlacedFootprint.Footprint.Height = ch;
        buf.PlacedFootprint.Footprint.Depth = 1;
        buf.PlacedFootprint.Footprint.RowPitch = tile.RowBytes;

        if (toResource) {
            ID3D12GraphicsCommandList_CopyTextureRegion(
                l->pList, &tex, x0, y0, 0, &buf, NULL);
        } else {
            D3D12_BOX box = {x0, y0, 0, x0 + cw, y0 + ch, 1};
            ID3D12GraphicsCommandList_CopyTextureRegion(
                l->pList, &buf, 0, 0, 0, &tex, &box);
        }

        /* Advance: box regions wrap at the box width, linear runs wrap
         * at the subresource's tile-grid width. */
        tx++;
        if (pSize->UseBox ? (tx >= pStart->X + bw) : (tx >= gridW)) {
            tx = pSize->UseBox ? pStart->X : 0;
            ty++;
        }
    }
}

static VOID APIENTRY
t12ResourceCopy(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HRESOURCE hDst,
                D3D12DDI_HRESOURCE hSrc)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_RESOURCE d = (PTRITON12_RESOURCE)hDst.pDrvPrivate;
    PTRITON12_RESOURCE s = (PTRITON12_RESOURCE)hSrc.pDrvPrivate;
    if (l && l->pList && d && d->pResource && s && s->pResource)
        ID3D12GraphicsCommandList_CopyResource(l->pList, d->pResource,
                                               s->pResource);
}



static VOID APIENTRY
t12IASetIndexBuffer(D3D12DDI_HCOMMANDLIST hList,
                    const D3D12DDI_INDEX_BUFFER_VIEW *pDesc)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList)
        /* Host GPU VAs, layout-identical struct: forward verbatim. */
        ID3D12GraphicsCommandList_IASetIndexBuffer(
            l->pList, (const D3D12_INDEX_BUFFER_VIEW *)pDesc);
}

static VOID APIENTRY
t12IASetVertexBuffers(D3D12DDI_HCOMMANDLIST hList, UINT StartSlot,
                      UINT NumViews, const D3D12DDI_VERTEX_BUFFER_VIEW *pViews)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList)
        ID3D12GraphicsCommandList_IASetVertexBuffers(
            l->pList, StartSlot, NumViews,
            (const D3D12_VERTEX_BUFFER_VIEW *)pViews);
}

static VOID APIENTRY
t12SOSetTargets(D3D12DDI_HCOMMANDLIST hList, UINT StartSlot, UINT NumViews,
                const D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW *pViews)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList)
        ID3D12GraphicsCommandList_SOSetTargets(
            l->pList, StartSlot, NumViews,
            (const D3D12_STREAM_OUTPUT_BUFFER_VIEW *)pViews);
}

static VOID APIENTRY
t12SetComputeRootDescriptorTable(D3D12DDI_HCOMMANDLIST hList, UINT Index,
                                 D3D12DDI_GPU_DESCRIPTOR_HANDLE Base)
{
    PTRITON12_LIST l = t12List(hList);
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = Base.ptr;
    if (l && l->pList)
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(l->pList, Index, h);
}

static VOID APIENTRY
t12SetGraphicsRootDescriptorTable(D3D12DDI_HCOMMANDLIST hList, UINT Index,
                                  D3D12DDI_GPU_DESCRIPTOR_HANDLE Base)
{
    PTRITON12_LIST l = t12List(hList);
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    h.ptr = Base.ptr;
    if (l && l->pList)
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(l->pList, Index, h);
}

static VOID APIENTRY
t12SetComputeRoot32BitConstant(D3D12DDI_HCOMMANDLIST hList, UINT Index,
                               UINT Data, UINT Offset)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList)
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(l->pList, Index, Data, Offset);
}

static VOID APIENTRY
t12SetGraphicsRoot32BitConstant(D3D12DDI_HCOMMANDLIST hList, UINT Index,
                                UINT Data, UINT Offset)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList)
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(l->pList, Index, Data, Offset);
}

static VOID APIENTRY
t12SetComputeRoot32BitConstants(D3D12DDI_HCOMMANDLIST hList, UINT Index,
                                UINT Num, const void *pData, UINT Offset)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList)
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(l->pList, Index, Num, pData, Offset);
}

static VOID APIENTRY
t12SetGraphicsRoot32BitConstants(D3D12DDI_HCOMMANDLIST hList, UINT Index,
                                 UINT Num, const void *pData, UINT Offset)
{
    PTRITON12_LIST l = t12List(hList);
    if (l && l->pList)
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l->pList, Index, Num, pData, Offset);
}

#define T12_ROOT_VIEW(name)                                                   \
static VOID APIENTRY                                                          \
t12##name(D3D12DDI_HCOMMANDLIST hList, UINT Index,                            \
          D3D12DDI_GPU_VIRTUAL_ADDRESS Va)                                    \
{                                                                             \
    PTRITON12_LIST l = t12List(hList);                                        \
    if (l && l->pList)                                                        \
        ID3D12GraphicsCommandList_##name(l->pList, Index, Va);                \
}

T12_ROOT_VIEW(SetComputeRootConstantBufferView)
T12_ROOT_VIEW(SetGraphicsRootConstantBufferView)
T12_ROOT_VIEW(SetComputeRootShaderResourceView)
T12_ROOT_VIEW(SetGraphicsRootShaderResourceView)
T12_ROOT_VIEW(SetComputeRootUnorderedAccessView)
T12_ROOT_VIEW(SetGraphicsRootUnorderedAccessView)

static VOID APIENTRY
t12ClearDepthStencilView(D3D12DDI_HCOMMANDLIST hList,
                         D3D12DDI_CPU_DESCRIPTOR_HANDLE View, UINT Flags,
                         FLOAT Depth, UINT8 Stencil, UINT NumRects,
                         const D3D12DDI_RECT *pRects)
{
    PTRITON12_LIST l = t12List(hList);
    D3D12_CPU_DESCRIPTOR_HANDLE v;
    v.ptr = (SIZE_T)View.ptr;
    if (l && l->pList)
        ID3D12GraphicsCommandList_ClearDepthStencilView(
            l->pList, v, (D3D12_CLEAR_FLAGS)Flags, Depth, Stencil, NumRects,
            (const D3D12_RECT *)pRects);
}

static VOID APIENTRY
t12ClearUnorderedAccessViewUint(D3D12DDI_HCOMMANDLIST hList,
                                D3D12DDI_GPU_DESCRIPTOR_HANDLE GpuHandle,
                                D3D12DDI_CPU_DESCRIPTOR_HANDLE CpuHandle,
                                D3D12DDI_HRESOURCE hResource,
                                const UINT Values[4], UINT NumRects,
                                const D3D12DDI_RECT *pRects)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    D3D12_GPU_DESCRIPTOR_HANDLE g; D3D12_CPU_DESCRIPTOR_HANDLE c;
    g.ptr = GpuHandle.ptr; c.ptr = (SIZE_T)CpuHandle.ptr;
    if (l && l->pList && r && r->pResource)
        ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(
            l->pList, g, c, r->pResource, Values, NumRects,
            (const D3D12_RECT *)pRects);
}

static VOID APIENTRY
t12ClearUnorderedAccessViewFloat(D3D12DDI_HCOMMANDLIST hList,
                                 D3D12DDI_GPU_DESCRIPTOR_HANDLE GpuHandle,
                                 D3D12DDI_CPU_DESCRIPTOR_HANDLE CpuHandle,
                                 D3D12DDI_HRESOURCE hResource,
                                 const FLOAT Values[4], UINT NumRects,
                                 const D3D12DDI_RECT *pRects)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    D3D12_GPU_DESCRIPTOR_HANDLE g; D3D12_CPU_DESCRIPTOR_HANDLE c;
    g.ptr = GpuHandle.ptr; c.ptr = (SIZE_T)CpuHandle.ptr;
    if (l && l->pList && r && r->pResource)
        ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(
            l->pList, g, c, r->pResource, Values, NumRects,
            (const D3D12_RECT *)pRects);
}

static VOID APIENTRY
t12BeginQuery(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HQUERYHEAP hHeap,
              D3D12DDI_QUERY_TYPE Type, UINT Index)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_QUERYHEAP q = (PTRITON12_QUERYHEAP)hHeap.pDrvPrivate;
    if (l && l->pList && q && q->pHeap)
        ID3D12GraphicsCommandList_BeginQuery(l->pList, q->pHeap,
                                             (D3D12_QUERY_TYPE)Type, Index);
}

static VOID APIENTRY
t12EndQuery(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HQUERYHEAP hHeap,
            D3D12DDI_QUERY_TYPE Type, UINT Index)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_QUERYHEAP q = (PTRITON12_QUERYHEAP)hHeap.pDrvPrivate;
    if (l && l->pList && q && q->pHeap)
        ID3D12GraphicsCommandList_EndQuery(l->pList, q->pHeap,
                                           (D3D12_QUERY_TYPE)Type, Index);
}

static VOID APIENTRY
t12ResolveQueryData(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HQUERYHEAP hHeap,
                    D3D12DDI_QUERY_TYPE Type, UINT Start, UINT Count,
                    D3D12DDI_HRESOURCE hDest, UINT64 Offset)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_QUERYHEAP q = (PTRITON12_QUERYHEAP)hHeap.pDrvPrivate;
    PTRITON12_RESOURCE d = (PTRITON12_RESOURCE)hDest.pDrvPrivate;
    if (l && l->pList && q && q->pHeap && d && d->pResource)
        ID3D12GraphicsCommandList_ResolveQueryData(
            l->pList, q->pHeap, (D3D12_QUERY_TYPE)Type, Start, Count,
            d->pResource, Offset);
}

static VOID APIENTRY
t12ResourceResolveSubresource(D3D12DDI_HCOMMANDLIST hList,
                              D3D12DDI_HRESOURCE hDst, UINT DstSub,
                              D3D12DDI_HRESOURCE hSrc, UINT SrcSub,
                              DXGI_FORMAT Format)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_RESOURCE d = (PTRITON12_RESOURCE)hDst.pDrvPrivate;
    PTRITON12_RESOURCE src = (PTRITON12_RESOURCE)hSrc.pDrvPrivate;
    if (l && l->pList && d && d->pResource && src && src->pResource)
        ID3D12GraphicsCommandList_ResolveSubresource(
            l->pList, d->pResource, DstSub, src->pResource, SrcSub, Format);
}

static VOID APIENTRY
t12DiscardResource(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HRESOURCE hResource,
                   const D3D12DDIARG_DISCARD_RESOURCE_0003 *pArgs)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    if (!l || !l->pList || !r || !r->pResource)
        return;
    D3D12_DISCARD_REGION region;
    region.NumRects = pArgs ? pArgs->NumRects : 0;
    region.pRects = pArgs ? (const D3D12_RECT *)pArgs->pRects : NULL;
    region.FirstSubresource = pArgs ? pArgs->FirstSubresource : 0;
    region.NumSubresources = pArgs ? pArgs->NumSubresources : 1;
    ID3D12GraphicsCommandList_DiscardResource(l->pList, r->pResource,
                                              pArgs ? &region : NULL);
}

static VOID APIENTRY
t12SetPredication(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HRESOURCE hBuffer,
                  UINT64 Offset, D3D12DDI_PREDICATION_OP Op)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hBuffer.pDrvPrivate;
    if (l && l->pList)
        ID3D12GraphicsCommandList_SetPredication(
            l->pList, (r && r->pResource) ? r->pResource : NULL, Offset,
            (D3D12_PREDICATION_OP)Op);
}

static VOID APIENTRY
t12ExecuteBundle(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HCOMMANDLIST hBundle)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_LIST b = t12List(hBundle);
    if (l && l->pList && b && b->pList)
        ID3D12GraphicsCommandList_ExecuteBundle(l->pList, b->pList);
}

static VOID APIENTRY
t12SetMarker(D3D12DDI_HCOMMANDLIST hList, UINT64 Marker)
{
    (void)hList; (void)Marker; /* profiling markers: no-op */
}

/* ---------- query heaps (device core) ---------- */

static SIZE_T APIENTRY
t12CalcPrivateQueryHeapSize(D3D12DDI_HDEVICE hDevice,
                            const D3D12DDIARG_CREATE_QUERY_HEAP_0001 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_QUERYHEAP);
}

static HRESULT APIENTRY
t12CreateQueryHeap(D3D12DDI_HDEVICE hDevice,
                   const D3D12DDIARG_CREATE_QUERY_HEAP_0001 *pArgs,
                   D3D12DDI_HQUERYHEAP hQueryHeap)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_QUERYHEAP q = (PTRITON12_QUERYHEAP)hQueryHeap.pDrvPrivate;
    if (!p || !p->pDev || !q || !pArgs)
        return E_INVALIDARG;
    memset(q, 0, sizeof(*q));
    D3D12_QUERY_HEAP_DESC desc;
    desc.Type = (D3D12_QUERY_HEAP_TYPE)pArgs->Type;
    desc.Count = pArgs->Count;
    desc.NodeMask = 0;
    HRESULT hr = ID3D12Device_CreateQueryHeap(
        p->pDev, &desc, &IID_ID3D12QueryHeap, (void **)&q->pHeap);
    TR_LOG("12.CreateQueryHeap: type=%d n=%u -> 0x%08lx",
           (int)pArgs->Type, pArgs->Count, (unsigned long)hr);
    return hr;
}

static VOID APIENTRY
t12DestroyQueryHeap(D3D12DDI_HDEVICE hDevice, D3D12DDI_HQUERYHEAP hQueryHeap)
{
    PTRITON12_QUERYHEAP q = (PTRITON12_QUERYHEAP)hQueryHeap.pDrvPrivate;
    (void)hDevice;
    if (q && q->pHeap) {
        ID3D12QueryHeap_Release(q->pHeap);
        q->pHeap = NULL;
    }
}

void
triton12InstallQueryDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnCalcPrivateQueryHeapSize = t12CalcPrivateQueryHeapSize;
    t->pfnCreateQueryHeap          = t12CreateQueryHeap;
    t->pfnDestroyQueryHeap         = t12DestroyQueryHeap;
}

/* ---------- _0025 .. _0033 list entries ----------
 *
 * Each is reachable only behind a cap this driver reports off
 * (DepthBoundsTest, ProgrammableSamplePositions, WriteBufferImmediate,
 * ViewInstancing, PROTECTED_RESOURCE_SESSION_SUPPORT), except
 * ResolveSubresourceRegion, which is a core API the runtime routes here
 * once the revision offers the slot. */

static VOID APIENTRY
t12OMSetDepthBounds(D3D12DDI_HCOMMANDLIST hList, FLOAT Min, FLOAT Max)
{
    PTRITON12_LIST l = t12List(hList);
    ID3D12GraphicsCommandList1 *l1 = NULL;
    if (!l || !l->pList)
        return;
    if (SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(
            l->pList, &IID_ID3D12GraphicsCommandList1, (void **)&l1))) {
        ID3D12GraphicsCommandList1_OMSetDepthBounds(l1, Min, Max);
        ID3D12GraphicsCommandList1_Release(l1);
    }
}

static VOID APIENTRY
t12SetSamplePositions(D3D12DDI_HCOMMANDLIST hList, UINT NumSamplesPerPixel,
                      UINT NumPixels, D3D12DDI_SAMPLE_POSITION *pPositions)
{
    (void)hList; (void)NumSamplesPerPixel; (void)NumPixels; (void)pPositions;
    TR_STUB("12.SetSamplePositions");
}

static VOID APIENTRY
t12ResourceResolveSubresourceRegion(D3D12DDI_HCOMMANDLIST hList,
                                    D3D12DDI_HRESOURCE hDst, UINT DstSub,
                                    UINT DstX, UINT DstY,
                                    D3D12DDI_HRESOURCE hSrc, UINT SrcSub,
                                    D3D12DDI_RECT *pSrcRect, DXGI_FORMAT Format,
                                    D3D12DDI_RESOLVE_MODE Mode)
{
    PTRITON12_LIST l = t12List(hList);
    PTRITON12_RESOURCE d = (PTRITON12_RESOURCE)hDst.pDrvPrivate;
    PTRITON12_RESOURCE src = (PTRITON12_RESOURCE)hSrc.pDrvPrivate;
    ID3D12GraphicsCommandList1 *l1 = NULL;
    if (!l || !l->pList || !d || !d->pResource || !src || !src->pResource)
        return;
    if (SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(
            l->pList, &IID_ID3D12GraphicsCommandList1, (void **)&l1))) {
        D3D12_RECT rc;
        if (pSrcRect) {
            rc.left = pSrcRect->left; rc.top = pSrcRect->top;
            rc.right = pSrcRect->right; rc.bottom = pSrcRect->bottom;
        }
        ID3D12GraphicsCommandList1_ResolveSubresourceRegion(
            l1, d->pResource, DstSub, DstX, DstY, src->pResource, SrcSub,
            pSrcRect ? &rc : NULL, Format, (D3D12_RESOLVE_MODE)Mode);
        ID3D12GraphicsCommandList1_Release(l1);
    } else {
        /* No region variant on the host list: whole-subresource resolve
         * is the nearest the backend offers. */
        ID3D12GraphicsCommandList_ResolveSubresource(
            l->pList, d->pResource, DstSub, src->pResource, SrcSub, Format);
    }
}

static VOID APIENTRY
t12SetProtectedResourceSession(D3D12DDI_HCOMMANDLIST hList,
                               D3D12DDI_HPROTECTEDRESOURCESESSION_0030 hSession)
{
    (void)hList; (void)hSession;
}

static VOID APIENTRY
t12WriteBufferImmediate(D3D12DDI_HCOMMANDLIST hList, UINT Count,
                        const D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032 *pParams,
                        const D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032 *pModes)
{
    (void)hList; (void)Count; (void)pParams; (void)pModes;
    TR_STUB("12.WriteBufferImmediate");
}

static VOID APIENTRY
t12SetViewInstanceMask(D3D12DDI_HCOMMANDLIST hList, UINT Mask)
{
    (void)hList; (void)Mask;
}

/* ---------- _0050 .. _0062 entries: none of these features is reported
 * (HARDWARE_SCHEDULING_CAPS 0, no meta commands, RaytracingTier 0,
 * VariableShadingRateTier 0, BackgroundProcessing off), so they answer
 * "none" where the runtime reads a count and are otherwise unreachable. */

static HRESULT APIENTRY
t12EnumerateMetaCommands(D3D12DDI_HDEVICE hDevice, UINT *pNum,
                         D3D12DDIARG_META_COMMAND_DESC *pDescs)
{
    (void)hDevice; (void)pDescs;
    if (pNum)
        *pNum = 0;
    return S_OK;
}

static HRESULT APIENTRY
t12EnumerateMetaCommandParameters(D3D12DDI_HDEVICE hDevice, GUID CommandId,
                                  D3D12DDI_META_COMMAND_PARAMETER_STAGE Stage,
                                  UINT *pCount,
                                  D3D12DDIARG_META_COMMAND_PARAMETER_DESC *pDescs)
{
    (void)hDevice; (void)CommandId; (void)Stage; (void)pDescs;
    if (pCount)
        *pCount = 0;
    return S_OK;
}

static SIZE_T APIENTRY
t12CalcPrivateMetaCommandSize(D3D12DDI_HDEVICE hDevice, GUID CommandId,
                              UINT NodeMask, const void *pParams, SIZE_T cb)
{
    (void)hDevice; (void)CommandId; (void)NodeMask; (void)pParams; (void)cb;
    return 0;
}

static HRESULT APIENTRY
t12CreateMetaCommand(D3D12DDI_HDEVICE hDevice, GUID CommandId, UINT NodeMask,
                     const void *pParams, SIZE_T cb,
                     D3D12DDI_HMETACOMMAND_0052 hMeta,
                     D3D12DDI_HRTMETACOMMAND_0052 hRTMeta)
{
    (void)hDevice; (void)CommandId; (void)NodeMask; (void)pParams; (void)cb;
    (void)hMeta; (void)hRTMeta;
    return E_NOTIMPL;
}

static VOID APIENTRY
t12DestroyMetaCommand(D3D12DDI_HDEVICE hDevice, D3D12DDI_HMETACOMMAND_0052 h)
{ (void)hDevice; (void)h; }

static VOID APIENTRY
t12GetMetaCommandRequiredParameterInfo(D3D12DDI_HMETACOMMAND_0052 h,
                                       D3D12DDI_META_COMMAND_PARAMETER_STAGE Stage,
                                       UINT Index,
                                       D3D12DDIARG_META_COMMAND_REQUIRED_PARAMETER_INFO *pInfo)
{
    (void)h; (void)Stage; (void)Index;
    if (pInfo)
        memset(pInfo, 0, sizeof(*pInfo));
}

static SIZE_T APIENTRY
t12CalcPrivateSchedulingGroupSize(D3D12DDI_HDEVICE hDevice,
                                  const D3D12DDIARG_CREATESCHEDULINGGROUP_0050 *pArgs)
{ (void)hDevice; (void)pArgs; return sizeof(void *); }

static HRESULT APIENTRY
t12CreateSchedulingGroup(D3D12DDI_HDEVICE hDevice,
                         const D3D12DDIARG_CREATESCHEDULINGGROUP_0050 *pArgs,
                         D3D12DDI_HSCHEDULINGGROUP_0050 h,
                         D3D12DDI_HRTSCHEDULINGGROUP_0050 hRT)
{ (void)hDevice; (void)pArgs; (void)h; (void)hRT; return E_NOTIMPL; }

static VOID APIENTRY
t12DestroySchedulingGroup(D3D12DDI_HDEVICE hDevice, D3D12DDI_HSCHEDULINGGROUP_0050 h)
{ (void)hDevice; (void)h; }

static SIZE_T APIENTRY
t12CalcPrivateStateObjectSize(D3D12DDI_HDEVICE hDevice,
                              const D3D12DDIARG_CREATE_STATE_OBJECT_0054 *pArgs)
{ (void)hDevice; (void)pArgs; return 0; }

static HRESULT APIENTRY
t12CreateStateObject(D3D12DDI_HDEVICE hDevice,
                     const D3D12DDIARG_CREATE_STATE_OBJECT_0054 *pArgs,
                     D3D12DDI_HSTATEOBJECT_0054 h, D3D12DDI_HRTSTATEOBJECT_0054 hRT)
{ (void)hDevice; (void)pArgs; (void)h; (void)hRT; return E_NOTIMPL; }

static VOID APIENTRY
t12DestroyStateObject(D3D12DDI_HDEVICE hDevice, D3D12DDI_HSTATEOBJECT_0054 h)
{ (void)hDevice; (void)h; }

static void APIENTRY
t12GetRaytracingAccelerationStructurePrebuildInfo(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_0054 *pInputs,
    D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_0054 *pInfo)
{
    (void)hDevice; (void)pInputs;
    if (pInfo)
        memset(pInfo, 0, sizeof(*pInfo));
}

static D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS APIENTRY
t12CheckDriverMatchingIdentifier(D3D12DDI_HDEVICE hDevice,
                                 D3D12DDI_SERIALIZED_DATA_TYPE Type,
                                 const D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054 *pId)
{
    (void)hDevice; (void)Type; (void)pId;
    return D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
}

static void *APIENTRY
t12GetShaderIdentifier(D3D12DDI_HSTATEOBJECT_0054 h, LPCWSTR pName)
{ (void)h; (void)pName; return NULL; }

static UINT APIENTRY
t12GetShaderStackSize(D3D12DDI_HSTATEOBJECT_0054 h, LPCWSTR pName)
{ (void)h; (void)pName; return 0; }

static UINT APIENTRY
t12GetPipelineStackSize(D3D12DDI_HSTATEOBJECT_0054 h)
{ (void)h; return 0; }

static void APIENTRY
t12SetPipelineStackSize(D3D12DDI_HSTATEOBJECT_0054 h, UINT size)
{ (void)h; (void)size; }

static void APIENTRY
t12SetBackgroundProcessingMode(D3D12DDI_HDEVICE hDevice,
                               D3D12DDI_BACKGROUND_PROCESSING_MODE_0062 Mode,
                               D3D12DDI_MEASUREMENTS_ACTION_0062 Action)
{ (void)hDevice; (void)Mode; (void)Action; }

static VOID APIENTRY
t12InitializeMetaCommand(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HMETACOMMAND_0052 h,
                         const void *p, SIZE_T cb)
{ (void)hList; (void)h; (void)p; (void)cb; }

static VOID APIENTRY
t12ExecuteMetaCommand(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HMETACOMMAND_0052 h,
                      const void *p, SIZE_T cb)
{ (void)hList; (void)h; (void)p; (void)cb; }

static VOID APIENTRY
t12BuildRaytracingAccelerationStructure(D3D12DDI_HCOMMANDLIST hList,
    const D3D12DDIARG_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_0054 *pArgs)
{ (void)hList; (void)pArgs; TR_STUB("12.BuildRaytracingAccelerationStructure"); }

static VOID APIENTRY
t12EmitRaytracingAccelerationStructurePostbuildInfo(D3D12DDI_HCOMMANDLIST hList,
    const D3D12DDIARG_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_0054 *pArgs)
{ (void)hList; (void)pArgs; TR_STUB("12.EmitRaytracingAccelerationStructurePostbuildInfo"); }

static VOID APIENTRY
t12CopyRaytracingAccelerationStructure(D3D12DDI_HCOMMANDLIST hList,
    const D3D12DDIARG_COPY_RAYTRACING_ACCELERATION_STRUCTURE_0054 *pArgs)
{ (void)hList; (void)pArgs; TR_STUB("12.CopyRaytracingAccelerationStructure"); }

static VOID APIENTRY
t12SetPipelineState1(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HSTATEOBJECT_0054 h)
{ (void)hList; (void)h; TR_STUB("12.SetPipelineState1"); }

static VOID APIENTRY
t12DispatchRays(D3D12DDI_HCOMMANDLIST hList, const D3D12DDIARG_DISPATCH_RAYS_0054 *pArgs)
{ (void)hList; (void)pArgs; TR_STUB("12.DispatchRays"); }

static VOID APIENTRY
t12RSSetShadingRate(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_SHADING_RATE_0062 Rate,
                    const D3D12DDI_SHADING_RATE_COMBINER_0062 *pCombiners)
{ (void)hList; (void)Rate; (void)pCombiners; }

static VOID APIENTRY
t12RSSetShadingRateImage(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HRESOURCE h)
{ (void)hList; (void)h; }

static SIZE_T APIENTRY
t12CalcPrivateAddToStateObjectSize(D3D12DDI_HDEVICE hDevice,
                                   const D3D12DDIARG_ADD_TO_STATE_OBJECT_0072 *pArgs)
{ (void)hDevice; (void)pArgs; return 0; }

static HRESULT APIENTRY
t12AddToStateObject(D3D12DDI_HDEVICE hDevice,
                    const D3D12DDIARG_ADD_TO_STATE_OBJECT_0072 *pArgs,
                    D3D12DDI_HSTATEOBJECT_0054 h, D3D12DDI_HRTSTATEOBJECT_0054 hRT)
{ (void)hDevice; (void)pArgs; (void)h; (void)hRT; return E_NOTIMPL; }

static void APIENTRY
t12SetBackgroundProcessingMode0063(D3D12DDI_HDEVICE hDevice,
                                   D3D12DDI_BACKGROUND_PROCESSING_MODE_0062 Mode,
                                   D3D12DDI_MEASUREMENTS_ACTION_0062 Action,
                                   BOOL *pbFurtherMeasurementsDesired)
{
    (void)hDevice; (void)Mode; (void)Action;
    if (pbFurtherMeasurementsDesired)
        *pbFurtherMeasurementsDesired = FALSE;
}

static void APIENTRY
t12ImplicitShaderCacheControl(D3D12DDI_HDEVICE hDevice,
                              D3D12DDI_IMPLICIT_SHADER_CACHE_CONTROL_FLAGS_0080 Flags)
{ (void)hDevice; (void)Flags; }

static VOID APIENTRY
t12DispatchMesh(D3D12DDI_HCOMMANDLIST hList, UINT x, UINT y, UINT z)
{ (void)hList; (void)x; (void)y; (void)z; TR_STUB("12.DispatchMesh"); }

void
triton12InstallListDeviceFuncs0080(D3D12DDI_DEVICE_FUNCS_CORE_0080 *t)
{
    t->pfnCalcPrivateAddToStateObjectSize = t12CalcPrivateAddToStateObjectSize;
    t->pfnAddToStateObject                = t12AddToStateObject;
    t->pfnSetBackgroundProcessingMode     = t12SetBackgroundProcessingMode0063;
    t->pfnImplicitShaderCacheControl      = t12ImplicitShaderCacheControl;
}

void
triton12InstallListFuncs0074(D3D12DDI_COMMAND_LIST_FUNCS_3D_0074 *t)
{
    t->pfnDispatchMesh = t12DispatchMesh;
}

void
triton12InstallListDeviceFuncs0062(D3D12DDI_DEVICE_FUNCS_CORE_0062 *t)
{
    t->pfnCalcPrivateSchedulingGroupSize = t12CalcPrivateSchedulingGroupSize;
    t->pfnCreateSchedulingGroup          = t12CreateSchedulingGroup;
    t->pfnDestroySchedulingGroup         = t12DestroySchedulingGroup;
    t->pfnEnumerateMetaCommands          = t12EnumerateMetaCommands;
    t->pfnEnumerateMetaCommandParameters = t12EnumerateMetaCommandParameters;
    t->pfnCalcPrivateMetaCommandSize     = t12CalcPrivateMetaCommandSize;
    t->pfnCreateMetaCommand              = t12CreateMetaCommand;
    t->pfnDestroyMetaCommand             = t12DestroyMetaCommand;
    t->pfnGetMetaCommandRequiredParameterInfo = t12GetMetaCommandRequiredParameterInfo;
    t->pfnCalcPrivateStateObjectSize     = t12CalcPrivateStateObjectSize;
    t->pfnCreateStateObject              = t12CreateStateObject;
    t->pfnDestroyStateObject             = t12DestroyStateObject;
    t->pfnGetRaytracingAccelerationStructurePrebuildInfo =
        t12GetRaytracingAccelerationStructurePrebuildInfo;
    t->pfnCheckDriverMatchingIdentifier  = t12CheckDriverMatchingIdentifier;
    t->pfnGetShaderIdentifier            = t12GetShaderIdentifier;
    t->pfnGetShaderStackSize             = t12GetShaderStackSize;
    t->pfnGetPipelineStackSize           = t12GetPipelineStackSize;
    t->pfnSetPipelineStackSize           = t12SetPipelineStackSize;
    t->pfnSetBackgroundProcessingMode    = t12SetBackgroundProcessingMode;
}

void
triton12InstallListFuncs0062(D3D12DDI_COMMAND_LIST_FUNCS_3D_0062 *t)
{
    t->pfnInitializeMetaCommand = t12InitializeMetaCommand;
    t->pfnExecuteMetaCommand    = t12ExecuteMetaCommand;
    t->pfnBuildRaytracingAccelerationStructure = t12BuildRaytracingAccelerationStructure;
    t->pfnEmitRaytracingAccelerationStructurePostbuildInfo =
        t12EmitRaytracingAccelerationStructurePostbuildInfo;
    t->pfnCopyRaytracingAccelerationStructure = t12CopyRaytracingAccelerationStructure;
    t->pfnSetPipelineState1     = t12SetPipelineState1;
    t->pfnDispatchRays          = t12DispatchRays;
    t->pfnRSSetShadingRate      = t12RSSetShadingRate;
    t->pfnRSSetShadingRateImage = t12RSSetShadingRateImage;
}

void
triton12InstallListDeviceFuncs0043(D3D12DDI_DEVICE_FUNCS_CORE_0043 *t)
{
    t->pfnCalcPrivateCommandPoolSize     = t12CalcPrivateCommandPoolSize;
    t->pfnCreateCommandPool              = t12CreateCommandPool;
    t->pfnDestroyCommandPool             = t12DestroyCommandPool;
    t->pfnResetCommandPool               = t12ResetCommandPool;
    t->pfnCalcPrivateCommandRecorderSize = t12CalcPrivateCommandRecorderSize;
    t->pfnCreateCommandRecorder          = t12CreateCommandRecorder;
    t->pfnDestroyCommandRecorder         = t12DestroyCommandRecorder;
    t->pfnCommandRecorderSetCommandPoolAsTarget =
        t12CommandRecorderSetCommandPoolAsTarget;
    t->pfnCalcPrivateCommandListSize     = t12CalcPrivateCommandListSize0040;
    t->pfnCreateCommandList              = t12CreateCommandList0040;
}

void
triton12InstallListFuncs0040(D3D12DDI_COMMAND_LIST_FUNCS_3D_0040 *t)
{
    t->pfnResetCommandList = t12ResetCommandList0040;
}

void
triton12InstallListFuncs0033(D3D12DDI_COMMAND_LIST_FUNCS_3D_0033 *t)
{
    t->pfnOMSetDepthBounds                 = t12OMSetDepthBounds;
    t->pfnSetSamplePositions               = t12SetSamplePositions;
    t->pfnResourceResolveSubresourceRegion = t12ResourceResolveSubresourceRegion;
    t->pfnSetProtectedResourceSession      = t12SetProtectedResourceSession;
    t->pfnWriteBufferImmediate             = t12WriteBufferImmediate;
    t->pfnSetViewInstanceMask              = t12SetViewInstanceMask;
}

void
triton12InstallListFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *t)
{
    t->pfnCloseCommandList  = t12CloseCommandList;
    t->pfnResetCommandList  = t12ResetCommandList;
    t->pfnCopyBufferRegion  = t12CopyBufferRegion;
    t->pfnResourceBarrier   = t12ResourceBarrier;
    t->pfnSetPipelineState  = t12SetPipelineState;
    t->pfnSetGraphicsRootSignature = t12SetGraphicsRootSignature;
    t->pfnSetComputeRootSignature  = t12SetComputeRootSignature;
    t->pfnSetDescriptorHeaps = t12SetDescriptorHeaps;
    t->pfnIaSetTopology     = t12IaSetTopology;
    t->pfnRsSetViewports    = t12RsSetViewports;
    t->pfnRsSetScissorRects = t12RsSetScissorRects;
    t->pfnOMSetRenderTargets = t12OMSetRenderTargets;
    t->pfnClearRenderTargetView = t12ClearRenderTargetView;
    t->pfnDrawInstanced     = t12DrawInstanced;
    t->pfnDrawIndexedInstanced = t12DrawIndexedInstanced;
    t->pfnDispatch          = t12Dispatch;
    t->pfnCopyTextureRegion = t12CopyTextureRegion;
    t->pfnCopyTiles         = t12CopyTiles;
    t->pfnResourceCopy      = t12ResourceCopy;
    t->pfnIASetIndexBuffer  = t12IASetIndexBuffer;
    t->pfnIASetVertexBuffers = t12IASetVertexBuffers;
    t->pfnSOSetTargets      = t12SOSetTargets;
    t->pfnSetComputeRootDescriptorTable  = t12SetComputeRootDescriptorTable;
    t->pfnSetGraphicsRootDescriptorTable = t12SetGraphicsRootDescriptorTable;
    t->pfnSetComputeRoot32BitConstant    = t12SetComputeRoot32BitConstant;
    t->pfnSetGraphicsRoot32BitConstant   = t12SetGraphicsRoot32BitConstant;
    t->pfnSetComputeRoot32BitConstants   = t12SetComputeRoot32BitConstants;
    t->pfnSetGraphicsRoot32BitConstants  = t12SetGraphicsRoot32BitConstants;
    t->pfnSetComputeRootConstantBufferView  = t12SetComputeRootConstantBufferView;
    t->pfnSetGraphicsRootConstantBufferView = t12SetGraphicsRootConstantBufferView;
    t->pfnSetComputeRootShaderResourceView  = t12SetComputeRootShaderResourceView;
    t->pfnSetGraphicsRootShaderResourceView = t12SetGraphicsRootShaderResourceView;
    t->pfnSetComputeRootUnorderedAccessView  = t12SetComputeRootUnorderedAccessView;
    t->pfnSetGraphicsRootUnorderedAccessView = t12SetGraphicsRootUnorderedAccessView;
    t->pfnClearDepthStencilView = t12ClearDepthStencilView;
    t->pfnClearUnorderedAccessViewUint  = t12ClearUnorderedAccessViewUint;
    t->pfnClearUnorderedAccessViewFloat = t12ClearUnorderedAccessViewFloat;
    t->pfnBeginQuery      = t12BeginQuery;
    t->pfnEndQuery        = t12EndQuery;
    t->pfnResolveQueryData = t12ResolveQueryData;
    t->pfnResourceResolveSubresource = t12ResourceResolveSubresource;
    t->pfnDiscardResource = t12DiscardResource;
    t->pfnSetPredication  = t12SetPredication;
    t->pfnExecuteBundle   = t12ExecuteBundle;
    t->pfnSetMarker       = t12SetMarker;
    t->pfnExecuteIndirect = t12ExecuteIndirect;
    t->pfnOmSetBlendFactor = t12OmSetBlendFactor;
    t->pfnOmSetStencilRef  = t12OmSetStencilRef;
    t->pfnClearRootArguments = t12ClearRootArguments;
}
