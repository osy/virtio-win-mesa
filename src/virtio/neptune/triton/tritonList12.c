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

enum { T12_LIST1, T12_LIST2, T12_LIST6, T12_LIST4 };

static void *
t12ListIface(PTRITON12_LIST l, UINT which)
{
    static const IID *const iids[] = {
        &IID_ID3D12GraphicsCommandList1,
        &IID_ID3D12GraphicsCommandList2,
        &IID_ID3D12GraphicsCommandList6,
        &IID_ID3D12GraphicsCommandList4,
    };
    if (!l || !l->pList)
        return NULL;
    if (!(l->IfaceTried & (1u << which))) {
        l->IfaceTried |= 1u << which;
        if (FAILED(ID3D12GraphicsCommandList_QueryInterface(
                l->pList, iids[which], &l->pIface[which])))
            l->pIface[which] = NULL;
    }
    return l->pIface[which];
}

/* Hand the runtime the list's dispatch table (direct or bundle), captured
 * in pfnFillDDITable. */
static void
t12ListSetTable(PTRITON12_DEVICE p, D3D12DDI_COMMAND_LIST_TYPE type,
                D3D12DDI_HRTCOMMANDLIST hRTList)
{
    if (!p->pUMCallbacks || !p->pUMCallbacks->pfnSetCommandListDDITableCb ||
        !p->pAdapter)
        return;
    const UINT idx = (type == D3D12DDI_COMMAND_LIST_TYPE_BUNDLE) ? 1 : 0;
    p->pUMCallbacks->pfnSetCommandListDDITableCb(
        hRTList, p->pAdapter->hRTTableCmdList[idx]);
}

/* A failed host Reset/Close on a VOID DDI: without pfnSetErrorCb the API
 * call returns S_OK and the failure surfaces only later (a failed Reset at
 * Close, a failed Close as device removal at ExecuteCommandLists).  Logged
 * unconditionally, since the per-call traces compile out by default. */
static void
t12ListError(PTRITON12_LIST l, const char *what, HRESULT hr)
{
    TR_LOG("12.%s FAILED hr=0x%08lx list=%p tid=%lu", what,
           (unsigned long)hr, (void *)l, (unsigned long)GetCurrentThreadId());
    if (l->pDev && l->pDev->pUMCallbacks && l->pDev->pUMCallbacks->pfnSetErrorCb)
        l->pDev->pUMCallbacks->pfnSetErrorCb(l->pDev->hRTDevice, hr);
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
    TR_LOG_HR(hr, "12.CreateCommandAllocator: type=%d qflags=0x%x -> 0x%08lx",
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
        TR_LOG_HR(hr, "12.CommandPool: allocator for type %d -> 0x%08lx", (int)type,
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
    TR_LOG_HR(hr, "12.CreateCommandList: type=%d qflags=0x%x -> 0x%08lx",
           (int)pArgs->Type, (unsigned)pArgs->QueueFlags, (unsigned long)hr);
    if (FAILED(hr))
        return hr;

    t12ListSetTable(p, pArgs->Type, pArgs->hRTCommandList);
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
     * registered.  Hold ScratchLock across create+Close so the
     * scratch allocator backs one recording list at a time. */
    const UINT si = t12PoolIndex(type);
    HRESULT hr;
    EnterCriticalSection(&p->ScratchLock);
    if (!p->pScratchAlloc[si]) {
        HRESULT ahr = ID3D12Device_CreateCommandAllocator(
            p->pDev, type, &IID_ID3D12CommandAllocator,
            (void **)&p->pScratchAlloc[si]);
        if (FAILED(ahr))
            p->pScratchAlloc[si] = NULL;
    }
    if (!p->pScratchAlloc[si]) {
        LeaveCriticalSection(&p->ScratchLock);
        return E_OUTOFMEMORY;
    }
    hr = ID3D12Device_CreateCommandList(
        p->pDev, 0, type, p->pScratchAlloc[si], NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&l->pList);
    if (SUCCEEDED(hr))
        ID3D12GraphicsCommandList_Close(l->pList);
    LeaveCriticalSection(&p->ScratchLock);

    TR_LOG_HR(hr, "12.CreateCommandList(0040): type=%d qflags=0x%x id=%llu -> 0x%08lx",
           (int)pArgs->Type, (unsigned)pArgs->QueueFlags,
           (unsigned long long)pArgs->ID, (unsigned long)hr);
    if (FAILED(hr))
        return hr;
    l->AwaitingReset = TRUE;

    t12ListSetTable(p, pArgs->Type, hRTList);
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
    if (FAILED(hr))
        t12ListError(l, "ResetCommandList(0040)", hr);
}

static VOID APIENTRY
t12DestroyCommandList(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDLIST hList)
{
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    (void)hDevice;
    if (!l)
        return;
    for (UINT i = 0; i < ARRAYSIZE(l->pIface); i++) {
        if (l->pIface[i])
            IUnknown_Release((IUnknown *)l->pIface[i]);
        l->pIface[i] = NULL;
    }
    l->IfaceTried = 0;
    if (l->pList) {
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
    if (FAILED(hr))
        t12ListError(l, "CloseCommandList", hr);
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
    if (FAILED(hr))
        t12ListError(l, "ResetCommandList", hr);
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

/* ID3D12GraphicsCommandList1::AtomicCopyBufferUINT{,64} arrive here, with
 * their dependent resources lowered to ATOMIC_COPY ranged barriers ahead
 * of the call.  The host list records those in order with this copy, so
 * the copy is forwarded without a dependency list. */
static VOID APIENTRY
t12AtomicCopyBufferRegion(D3D12DDI_HCOMMANDLIST hList,
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
    ID3D12GraphicsCommandList1 *l1 = t12ListIface(l, T12_LIST1);
    if (!l1 || (SrcBytes != 4 && SrcBytes != 8)) {
        ID3D12GraphicsCommandList_CopyBufferRegion(
            l->pList, dst->pResource, Dst.BaseAddress.UMD.Offset,
            src->pResource, Src.BaseAddress.UMD.Offset, SrcBytes);
        return;
    }
    if (SrcBytes == 8)
        ID3D12GraphicsCommandList1_AtomicCopyBufferUINT64(
            l1, dst->pResource, Dst.BaseAddress.UMD.Offset,
            src->pResource, Src.BaseAddress.UMD.Offset, 0, NULL, NULL);
    else
        ID3D12GraphicsCommandList1_AtomicCopyBufferUINT(
            l1, dst->pResource, Dst.BaseAddress.UMD.Offset,
            src->pResource, Src.BaseAddress.UMD.Offset, 0, NULL, NULL);
}

static VOID APIENTRY
t12ResourceBarrier(D3D12DDI_HCOMMANDLIST hList, UINT Count,
                   const D3D12DDIARG_RESOURCE_BARRIER_0022 *pBarriers)
{
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    if (!l || !l->pList || !Count || !pBarriers)
        return;

    /* Forwarded in stack-sized chunks: a heap allocation that could fail
     * would drop every barrier of the call. */
    D3D12_RESOURCE_BARRIER bar[32];
    UINT n = 0;
    for (UINT i = 0; i < Count; i++) {
        if (n == ARRAYSIZE(bar)) {
            ID3D12GraphicsCommandList_ResourceBarrier(l->pList, n, bar);
            n = 0;
        }
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
            /* Non-aliasing ranged barriers (atomic-copy dependencies, see
             * t12AtomicCopyBufferRegion) have no API equivalent;
             * submission order covers them. */
            break;
        }
        default:
            break;
        }
    }
    if (n)
        ID3D12GraphicsCommandList_ResourceBarrier(l->pList, n, bar);
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
    TR_LOG_HR(hr, "12.CreateCommandSignature: stride=%u nargs=%u rs=%p -> 0x%08lx",
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
    TR_LOG_HOT("12.OMSetRenderTargets: num=%u single=%d dsv=%d", Num, (int)SingleHandle,
           (int)(pDSV != NULL));
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
    TR_LOG_HR(hr, "12.CreateQueryHeap: type=%d n=%u -> 0x%08lx",
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
 * OMSetDepthBounds, ResolveSubresourceRegion and WriteBufferImmediate
 * forward to the host list.  The rest stay stubs behind caps reported
 * off, which is safe only where the unsupported path is documented as
 * drop/sanitize (SetSamplePositions, the view-instance mask) or the
 * feature needs a create-time object that cannot exist (protected
 * session).  A zero cap does not make an entry unreachable: the runtime
 * still routes the call here, and for a cap it validates against
 * (WriteBufferImmediate) a zero report latches E_INVALIDARG on the
 * recording list instead of dropping the call. */

static VOID APIENTRY
t12OMSetDepthBounds(D3D12DDI_HCOMMANDLIST hList, FLOAT Min, FLOAT Max)
{
    ID3D12GraphicsCommandList1 *l1 = t12ListIface(t12List(hList), T12_LIST1);
    if (l1)
        ID3D12GraphicsCommandList1_OMSetDepthBounds(l1, Min, Max);
}

static VOID APIENTRY
t12SetSamplePositions(D3D12DDI_HCOMMANDLIST hList, UINT NumSamplesPerPixel,
                      UINT NumPixels, D3D12DDI_SAMPLE_POSITION *pPositions)
{
    /* D3D12DDI_SAMPLE_POSITION and D3D12_SAMPLE_POSITION are both
     * {INT8 X; INT8 Y}, so the array passes through unchanged. */
    ID3D12GraphicsCommandList1 *l1 = t12ListIface(t12List(hList), T12_LIST1);
    if (!l1)
        return;
    TR_LOG_HOT("12.SetSamplePositions: samples=%u pixels=%u first=(%d,%d)",
           NumSamplesPerPixel, NumPixels,
           (NumSamplesPerPixel && NumPixels && pPositions) ? pPositions[0].X : 0,
           (NumSamplesPerPixel && NumPixels && pPositions) ? pPositions[0].Y : 0);
    ID3D12GraphicsCommandList1_SetSamplePositions(
        l1, NumSamplesPerPixel, NumPixels,
        (D3D12_SAMPLE_POSITION *)pPositions);
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
    if (!l || !l->pList || !d || !d->pResource || !src || !src->pResource)
        return;
    TR_LOG_HOT("12.ResolveSubresourceRegion: dst=%p sub=%u (%u,%u) src=%p sub=%u fmt=%d mode=%d",
           (void *)d, DstSub, DstX, DstY, (void *)src, SrcSub, (int)Format, (int)Mode);
    ID3D12GraphicsCommandList1 *l1 = t12ListIface(l, T12_LIST1);
    if (l1) {
        D3D12_RECT rc;
        if (pSrcRect) {
            rc.left = pSrcRect->left; rc.top = pSrcRect->top;
            rc.right = pSrcRect->right; rc.bottom = pSrcRect->bottom;
        }
        ID3D12GraphicsCommandList1_ResolveSubresourceRegion(
            l1, d->pResource, DstSub, DstX, DstY, src->pResource,
            SrcSub, pSrcRect ? &rc : NULL, Format, (D3D12_RESOLVE_MODE)Mode);
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

/* The DDI parameter and mode types are layout-identical to the API ones
 * ({Dest GPU VA, UINT32 Value}; DEFAULT/MARKER_IN/MARKER_OUT with the
 * same values), so the arrays pass through by cast.  Dest needs no
 * translation: GetGPUVirtualAddress answers come from the host. */
C_ASSERT(sizeof(D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032) ==
         sizeof(D3D12_WRITEBUFFERIMMEDIATE_PARAMETER));
C_ASSERT(sizeof(D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032) ==
         sizeof(D3D12_WRITEBUFFERIMMEDIATE_MODE));

static VOID APIENTRY
t12WriteBufferImmediate(D3D12DDI_HCOMMANDLIST hList, UINT Count,
                        const D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032 *pParams,
                        const D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032 *pModes)
{
    PTRITON12_LIST l = t12List(hList);
    if (!l || !l->pList || !Count || !pParams)
        return;
    ID3D12GraphicsCommandList2 *l2 = t12ListIface(l, T12_LIST2);
    if (l2) {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            TR_LOG("12.WriteBufferImmediate: forwarding to the host "
                   "(count=%u mode0=%u)", Count,
                   pModes ? (unsigned)pModes[0] : 0u);
        ID3D12GraphicsCommandList2_WriteBufferImmediate(
            l2, Count,
            (const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *)pParams,
            (const D3D12_WRITEBUFFERIMMEDIATE_MODE *)pModes);
    } else {
        /* OPTIONS reports the host's queue support, so dropping the
         * writes here is silent data loss.  Say so once. */
        static LONG qiFail;
        if (!InterlockedExchange(&qiFail, 1))
            TR_LOG("12.WriteBufferImmediate: host list has no "
                   "ID3D12GraphicsCommandList2 -- writes DROPPED");
    }
}

static VOID APIENTRY
t12SetViewInstanceMask(D3D12DDI_HCOMMANDLIST hList, UINT Mask)
{
    (void)hList; (void)Mask;
}

/* ---------- _0050 .. _0062 entries: most of these features are not
 * reported (HARDWARE_SCHEDULING_CAPS 0, no meta commands,
 * VariableShadingRateTier 0, BackgroundProcessing off), so they answer
 * "none" where the runtime reads a count and are otherwise unreachable.
 * Raytracing forwards to the host and is only reported when enabled (see
 * triton12DxrEnabled). */

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

static ID3D12Device5 *
t12Device5(PTRITON12_DEVICE p)
{
    return p ? p->pDev5 : NULL;
}

/* Synchronous on the wire: the host answers with the sizes. */
static void APIENTRY
t12GetRaytracingAccelerationStructurePrebuildInfo(
    D3D12DDI_HDEVICE hDevice,
    const D3D12DDI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_0054 *pInputs,
    D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_0054 *pInfo)
{
    if (!pInfo)
        return;
    memset(pInfo, 0, sizeof(*pInfo));
    ID3D12Device5 *d5 = t12Device5(triton12Device(hDevice));
    if (!d5 || !pInputs)
        return;
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(
        d5, (const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *)pInputs,
        (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *)pInfo);
    TR_LOG_HOT("12.GetRaytracingAccelerationStructurePrebuildInfo: type=%d n=%u -> "
               "result=%llu scratch=%llu", (int)pInputs->Type, pInputs->NumDescs,
               (unsigned long long)pInfo->ResultDataMaxSizeInBytes,
               (unsigned long long)pInfo->ScratchDataSizeInBytes);
}

static D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS APIENTRY
t12CheckDriverMatchingIdentifier(D3D12DDI_HDEVICE hDevice,
                                 D3D12DDI_SERIALIZED_DATA_TYPE Type,
                                 const D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054 *pId)
{
    ID3D12Device5 *d5 = t12Device5(triton12Device(hDevice));
    if (!d5 || !pId)
        return D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
    return (D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS)
        ID3D12Device5_CheckDriverMatchingIdentifier(
            d5, (D3D12_SERIALIZED_DATA_TYPE)Type,
            (const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *)pId);
}

static ID3D12GraphicsCommandList4 *t12List4(D3D12DDI_HCOMMANDLIST hList, const char *what);

/* ---------- DXR state objects ----------
 *
 * DDI -> API translation of state-object descs.  The DDI desc differs from
 * the API one in three ways:
 *   - handles (root signatures, existing collections) instead of
 *     interfaces;
 *   - DXIL libraries arrive as a token pointer with no length (a full
 *     container, or only its DXIL program part);
 *   - there are no SUBOBJECT_TO_EXPORTS_ASSOCIATIONs: the runtime has
 *     resolved every association (explicit and default) and hands a
 *     SHADER_EXPORT_SUMMARY listing, per exported function, the subobjects
 *     that apply to it.  The API desc is rebuilt with one explicit
 *     association per local root signature / shader config / global root
 *     signature naming exactly the functions the summary gives it, so the
 *     host resolves the same thing the runtime did.  A subobject of those
 *     types that no function uses is dropped (it would otherwise become a
 *     default for everything), and one that is the only one of its type and
 *     used by every function is left as the default.
 * Shader identifiers are opaque host data: fetched over the transport
 * (npt_com_get_shader_identifier) on first use and cached per state object
 * for its lifetime, since the runtime hands the pointer back to the app. */

#include <wchar.h>

extern HRESULT npt_com_get_shader_identifier(void *props, const WCHAR *name,
                                             void *out_identifier);

typedef struct TRITON12_SOID {
    struct TRITON12_SOID *next;
    WCHAR *name;
    BOOL ok;
    BYTE id[32];
} TRITON12_SOID;

typedef struct TRITON12_STATEOBJECT {
    ID3D12StateObject *pSO;
    ID3D12StateObjectProperties *pProps;
    SRWLOCK lock;
    TRITON12_SOID *ids;
} TRITON12_STATEOBJECT, *PTRITON12_STATEOBJECT;

static PTRITON12_STATEOBJECT
t12StateObject(D3D12DDI_HSTATEOBJECT_0054 h)
{
    return (PTRITON12_STATEOBJECT)h.pDrvPrivate;
}

C_ASSERT(sizeof(D3D12DDI_EXPORT_DESC_0054) == sizeof(D3D12_EXPORT_DESC));
C_ASSERT(sizeof(D3D12DDIARG_DISPATCH_RAYS_0054) == sizeof(D3D12_DISPATCH_RAYS_DESC));

/* Base of the committed, readable region holding p, or NULL. */
static const BYTE *
t12ReadableBase(const BYTE *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return NULL;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return NULL;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)))
        return NULL;
    return (const BYTE *)mbi.BaseAddress;
}

static BOOL
t12ReadableRange(const BYTE *lo, const BYTE *hi)
{
    while (lo < hi) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!t12ReadableBase(lo) || !VirtualQuery(lo, &mbi, sizeof(mbi)))
            return FALSE;
        lo = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
    return TRUE;
}

static UINT
t12Rd32(const BYTE *p)
{
    UINT v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* The runtime hands the DDI only the DXIL program part of a library, but
 * the host (D3DMetal) needs the container's RDAT part to enumerate the
 * library's functions.  The part pointer normally still points into the
 * application's container, so walk back to its header.  Every byte read
 * is checked against VirtualQuery first. */
static const BYTE *
t12EnclosingContainer(const UINT *tok, UINT *pcb)
{
    const BYTE *part = (const BYTE *)tok - 8;
    const UINT cbProg = tok[1] * sizeof(UINT);
    if (!t12ReadableRange(part, (const BYTE *)tok))
        return NULL;
    if (t12Rd32(part) != 0x4C495844u /* 'DXIL' */ || t12Rd32(part + 4) < cbProg)
        return NULL;
    const UINT cbPart = t12Rd32(part + 4);
    const BYTE *lo = t12ReadableBase(part);
    for (SIZE_T back = 32 + 4; back <= (4u << 20); back += 4) {
        const BYTE *c = part - back;
        if (c > part)
            return NULL;    /* wrapped */
        if (c < lo) {
            lo = t12ReadableBase(c);
            if (!lo)
                return NULL;
        }
        if (t12Rd32(c) != 0x43425844u /* 'DXBC' */)
            continue;
        const UINT cb = t12Rd32(c + 24), n = t12Rd32(c + 28);
        if (n == 0 || n > 64 || cb < 32 + 4 * n || cb > (256u << 20) ||
            (SIZE_T)cb < back + 8 + cbPart)
            continue;
        if (!t12ReadableRange(c, c + cb))
            return NULL;
        for (UINT i = 0; i < n; i++) {
            if (t12Rd32(c + 32 + 4 * i) == (UINT)back) {
                *pcb = cb;
                return c;
            }
        }
    }
    return NULL;
}

/* Copy of a DXIL library in container form.  The runtime hands the DDI
 * either the whole container or only its DXIL program part. */
static void *
t12DxilLibraryContainer(const UINT *tok, SIZE_T *pcb)
{
    const BYTE *c = NULL;
    UINT cb = 0;
    *pcb = 0;
    if (!tok)
        return NULL;
    if (tok[0] == 0x43425844u /* 'DXBC' */) {
        cb = tok[6];
        if (cb >= 32 && cb <= (256u << 20))
            c = (const BYTE *)tok;
    } else if (tok[2] == 0x4C495844u /* bare DXIL program part */) {
        c = t12EnclosingContainer(tok, &cb);
    }
    if (!c) {
        TR_LOG("12.StateObject: DXIL library without a usable container "
               "(header %08x %08x %08x)", tok[0], tok[1], tok[2]);
        return NULL;
    }
    void *copy = malloc(cb);
    if (copy) {
        memcpy(copy, c, cb);
        *pcb = cb;
    }
    return copy;
}

/* Everything an API desc built from a DDI one points into. */
typedef struct T12_SO_BUILD {
    D3D12_STATE_SUBOBJECT *subs;
    UINT nsubs;
    void **blocks;          /* malloc'd descs / containers / name arrays */
    UINT nblocks, capblocks;
} T12_SO_BUILD;

/* Takes ownership of p (freed with the build). */
static BOOL
t12SoOwn(T12_SO_BUILD *b, void *p)
{
    if (b->nblocks == b->capblocks) {
        UINT cap = b->capblocks ? b->capblocks * 2 : 32;
        void **nb = (void **)realloc(b->blocks, cap * sizeof(void *));
        if (!nb)
            return FALSE;
        b->blocks = nb;
        b->capblocks = cap;
    }
    b->blocks[b->nblocks++] = p;
    return TRUE;
}

static void *
t12SoAlloc(T12_SO_BUILD *b, SIZE_T cb)
{
    void *p = calloc(1, cb ? cb : 1);
    if (p && !t12SoOwn(b, p)) {
        free(p);
        return NULL;
    }
    return p;
}

static void
t12SoBuildFree(T12_SO_BUILD *b)
{
    for (UINT i = 0; i < b->nblocks; i++)
        free(b->blocks[i]);
    free(b->blocks);
    free(b->subs);
    memset(b, 0, sizeof(*b));
}

static BOOL
t12SoAssociable(D3D12DDI_STATE_SUBOBJECT_TYPE t)
{
    return t == D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE ||
           t == D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE ||
           t == D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
}

static HRESULT
t12BuildStateObjectDesc(UINT n, const D3D12DDI_STATE_SUBOBJECT_0054 *src,
                        T12_SO_BUILD *b)
{
    memset(b, 0, sizeof(*b));
    if (n && !src)
        return E_INVALIDARG;
    /* worst case: every subobject plus one association per subobject */
    b->subs = (D3D12_STATE_SUBOBJECT *)calloc(2 * (SIZE_T)n + 1, sizeof(*b->subs));
    UINT *apiIndex = (UINT *)malloc(((SIZE_T)n + 1) * sizeof(UINT));
    UINT *uses = (UINT *)calloc((SIZE_T)n + 1, sizeof(UINT));
    if (!b->subs || !apiIndex || !uses) {
        free(apiIndex); free(uses);
        return E_OUTOFMEMORY;
    }
    HRESULT hr = S_OK;
    const D3D12DDI_FUNCTION_SUMMARY_0054 *summary = NULL;

    /* association usage counts from the summary (to drop unused ones) */
    for (UINT i = 0; i < n; i++)
        if (src[i].Type == D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY)
            summary = (const D3D12DDI_FUNCTION_SUMMARY_0054 *)src[i].pDesc;
    if (summary) {
        for (UINT f = 0; f < summary->NumExportedFunctions; f++) {
            const D3D12DDI_FUNCTION_SUMMARY_NODE_0054 *node = &summary->pSummaries[f];
            for (UINT k = 0; k < node->NumAssociatedSubobjects; k++) {
                const D3D12DDI_STATE_SUBOBJECT_0054 *s = node->ppAssociatedSubobjects[k];
                if (s >= src && s < src + n)
                    uses[s - src]++;
            }
        }
    }

    for (UINT i = 0; i < n && SUCCEEDED(hr); i++) {
        apiIndex[i] = UINT_MAX;
        const void *d = src[i].pDesc;
        D3D12_STATE_SUBOBJECT *o = &b->subs[b->nsubs];
        if (t12SoAssociable(src[i].Type) && summary && !uses[i])
            continue;
        switch (src[i].Type) {
        case D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG: {
            D3D12_STATE_OBJECT_CONFIG *c = t12SoAlloc(b, sizeof(*c));
            if (!c) { hr = E_OUTOFMEMORY; break; }
            c->Flags = (D3D12_STATE_OBJECT_FLAGS)((const D3D12DDI_STATE_OBJECT_CONFIG_0054 *)d)->Flags;
            o->Type = D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG; o->pDesc = c;
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE: {
            D3D12_GLOBAL_ROOT_SIGNATURE *c = t12SoAlloc(b, sizeof(*c));
            if (!c) { hr = E_OUTOFMEMORY; break; }
            c->pGlobalRootSignature =
                triton12RootSig(((const D3D12DDI_GLOBAL_ROOT_SIGNATURE_0054 *)d)->hGlobalRootSignature);
            o->Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE; o->pDesc = c;
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
            D3D12_LOCAL_ROOT_SIGNATURE *c = t12SoAlloc(b, sizeof(*c));
            if (!c) { hr = E_OUTOFMEMORY; break; }
            c->pLocalRootSignature =
                triton12RootSig(((const D3D12DDI_LOCAL_ROOT_SIGNATURE_0054 *)d)->hLocalRootSignature);
            o->Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE; o->pDesc = c;
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_NODE_MASK: {
            D3D12_NODE_MASK *c = t12SoAlloc(b, sizeof(*c));
            if (!c) { hr = E_OUTOFMEMORY; break; }
            c->NodeMask = ((const D3D12DDI_NODE_MASK_0054 *)d)->NodeMask;
            o->Type = D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK; o->pDesc = c;
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
            const D3D12DDI_DXIL_LIBRARY_DESC_0054 *s = (const D3D12DDI_DXIL_LIBRARY_DESC_0054 *)d;
            D3D12_DXIL_LIBRARY_DESC *c = t12SoAlloc(b, sizeof(*c));
            if (!c) { hr = E_OUTOFMEMORY; break; }
            SIZE_T cb = 0;
            void *lib = t12DxilLibraryContainer(s->pDXILLibrary, &cb);
            if (!lib || !t12SoOwn(b, lib)) { free(lib); hr = E_INVALIDARG; break; }
            c->DXILLibrary.pShaderBytecode = lib;
            c->DXILLibrary.BytecodeLength = cb;
            c->NumExports = s->NumExports;
            c->pExports = (D3D12_EXPORT_DESC *)s->pExports;
            o->Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY; o->pDesc = c;
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
            const D3D12DDI_EXISTING_COLLECTION_DESC_0054 *s = (const D3D12DDI_EXISTING_COLLECTION_DESC_0054 *)d;
            PTRITON12_STATEOBJECT coll = t12StateObject(s->hExistingCollection);
            D3D12_EXISTING_COLLECTION_DESC *c = t12SoAlloc(b, sizeof(*c));
            if (!c || !coll || !coll->pSO) { hr = c ? E_INVALIDARG : E_OUTOFMEMORY; break; }
            c->pExistingCollection = coll->pSO;
            c->NumExports = s->NumExports;
            c->pExports = (D3D12_EXPORT_DESC *)s->pExports;
            o->Type = D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION; o->pDesc = c;
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: {
            D3D12_RAYTRACING_SHADER_CONFIG *c = t12SoAlloc(b, sizeof(*c));
            if (!c) { hr = E_OUTOFMEMORY; break; }
            const D3D12DDI_RAYTRACING_SHADER_CONFIG_0054 *s = d;
            c->MaxPayloadSizeInBytes = s->MaxPayloadSizeInBytes;
            c->MaxAttributeSizeInBytes = s->MaxAttributeSizeInBytes;
            o->Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG; o->pDesc = c;
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: {
            /* This driver reports a DDI >= _0075, so the runtime hands
             * the _0075 desc (depth + flags). */
            const D3D12DDI_RAYTRACING_PIPELINE_CONFIG_0075 *s = d;
            if (s->Flags) {
                D3D12_RAYTRACING_PIPELINE_CONFIG1 *c = t12SoAlloc(b, sizeof(*c));
                if (!c) { hr = E_OUTOFMEMORY; break; }
                c->MaxTraceRecursionDepth = s->MaxTraceRecursionDepth;
                c->Flags = (D3D12_RAYTRACING_PIPELINE_FLAGS)s->Flags;
                o->Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1; o->pDesc = c;
            } else {
                D3D12_RAYTRACING_PIPELINE_CONFIG *c = t12SoAlloc(b, sizeof(*c));
                if (!c) { hr = E_OUTOFMEMORY; break; }
                c->MaxTraceRecursionDepth = s->MaxTraceRecursionDepth;
                o->Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG; o->pDesc = c;
            }
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
            const D3D12DDI_HIT_GROUP_DESC_0054 *s = d;
            D3D12_HIT_GROUP_DESC *c = t12SoAlloc(b, sizeof(*c));
            if (!c) { hr = E_OUTOFMEMORY; break; }
            c->HitGroupExport = s->HitGroupExport;
            c->Type = (D3D12_HIT_GROUP_TYPE)s->Type;
            c->AnyHitShaderImport = s->AnyHitShaderImport;
            c->ClosestHitShaderImport = s->ClosestHitShaderImport;
            c->IntersectionShaderImport = s->IntersectionShaderImport;
            o->Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP; o->pDesc = c;
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY:
            continue;   /* consumed below */
        default:
            TR_LOG("12.StateObject: subobject type %d not supported", (int)src[i].Type);
            hr = E_NOTIMPL;
            break;
        }
        if (SUCCEEDED(hr))
            apiIndex[i] = b->nsubs++;
    }

    /* explicit associations rebuilt from the summary */
    if (SUCCEEDED(hr) && summary) {
        for (UINT i = 0; i < n && SUCCEEDED(hr); i++) {
            if (!t12SoAssociable(src[i].Type) || !uses[i] || apiIndex[i] == UINT_MAX)
                continue;
            /* The sole subobject of its type used by every exported function
             * is what a default association means.  Spelling it out is legal
             * but D3DMetal does not resolve such associations, so keep
             * explicit ones only for subsets or ambiguity. */
            {
                UINT same = 0;
                for (UINT j = 0; j < n; j++)
                    if (src[j].Type == src[i].Type && apiIndex[j] != UINT_MAX)
                        same++;
                if (same == 1 && uses[i] >= summary->NumExportedFunctions)
                    continue;
            }
            LPCWSTR *names = t12SoAlloc(b, uses[i] * sizeof(LPCWSTR));
            D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *a = t12SoAlloc(b, sizeof(*a));
            if (!names || !a) { hr = E_OUTOFMEMORY; break; }
            UINT cnt = 0;
            for (UINT f = 0; f < summary->NumExportedFunctions; f++) {
                const D3D12DDI_FUNCTION_SUMMARY_NODE_0054 *node = &summary->pSummaries[f];
                for (UINT k = 0; k < node->NumAssociatedSubobjects; k++)
                    if (node->ppAssociatedSubobjects[k] == &src[i] && cnt < uses[i])
                        names[cnt++] = node->ExportNameUnmangled;
            }
            a->pSubobjectToAssociate = &b->subs[apiIndex[i]];
            a->NumExports = cnt;
            a->pExports = names;
            b->subs[b->nsubs].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
            b->subs[b->nsubs].pDesc = a;
            b->nsubs++;
        }
    }
    free(apiIndex);
    free(uses);
    return hr;
}

static void
t12StateObjectRelease(PTRITON12_STATEOBJECT so)
{
    if (!so)
        return;
    for (TRITON12_SOID *e = so->ids, *nx; e; e = nx) {
        nx = e->next;
        free(e->name);
        free(e);
    }
    so->ids = NULL;
    if (so->pProps) { ID3D12StateObjectProperties_Release(so->pProps); so->pProps = NULL; }
    if (so->pSO) { ID3D12StateObject_Release(so->pSO); so->pSO = NULL; }
}

static HRESULT
t12StateObjectFinish(PTRITON12_STATEOBJECT so, HRESULT hr, const char *what, UINT nsubs)
{
    if (SUCCEEDED(hr) && so->pSO) {
        hr = ID3D12StateObject_QueryInterface(so->pSO, &IID_ID3D12StateObjectProperties,
                                              (void **)&so->pProps);
        if (FAILED(hr))
            so->pProps = NULL;
    }
    TR_LOG_HR(hr, "12.%s: %u API subobjects -> 0x%08lx", what, nsubs, (unsigned long)hr);
    if (FAILED(hr))
        t12StateObjectRelease(so);
    return hr;
}

static SIZE_T APIENTRY
t12CalcPrivateStateObjectSize(D3D12DDI_HDEVICE hDevice,
                              const D3D12DDIARG_CREATE_STATE_OBJECT_0054 *pArgs)
{ (void)hDevice; (void)pArgs; return sizeof(TRITON12_STATEOBJECT); }

static HRESULT APIENTRY
t12CreateStateObject(D3D12DDI_HDEVICE hDevice,
                     const D3D12DDIARG_CREATE_STATE_OBJECT_0054 *pArgs,
                     D3D12DDI_HSTATEOBJECT_0054 h, D3D12DDI_HRTSTATEOBJECT_0054 hRT)
{
    (void)hRT;
    PTRITON12_STATEOBJECT so = t12StateObject(h);
    ID3D12Device5 *d5 = t12Device5(triton12Device(hDevice));
    if (!so || !pArgs)
        return E_INVALIDARG;
    memset(so, 0, sizeof(*so));
    InitializeSRWLock(&so->lock);
    if (!d5)
        return E_NOTIMPL;
    T12_SO_BUILD b;
    HRESULT hr = t12BuildStateObjectDesc(pArgs->NumSubobjects, pArgs->pSubobjects, &b);
    if (SUCCEEDED(hr)) {
        D3D12_STATE_OBJECT_DESC desc;
        desc.Type = (D3D12_STATE_OBJECT_TYPE)pArgs->Type;
        desc.NumSubobjects = b.nsubs;
        desc.pSubobjects = b.subs;
        hr = ID3D12Device5_CreateStateObject(d5, &desc, &IID_ID3D12StateObject, (void **)&so->pSO);
    }
    UINT nsubs = b.nsubs;
    t12SoBuildFree(&b);
    return t12StateObjectFinish(so, hr, "CreateStateObject", nsubs);
}

static VOID APIENTRY
t12DestroyStateObject(D3D12DDI_HDEVICE hDevice, D3D12DDI_HSTATEOBJECT_0054 h)
{ (void)hDevice; t12StateObjectRelease(t12StateObject(h)); }

static SIZE_T APIENTRY
t12CalcPrivateAddToStateObjectSize(D3D12DDI_HDEVICE hDevice,
                                   const D3D12DDIARG_ADD_TO_STATE_OBJECT_0072 *pArgs)
{ (void)hDevice; (void)pArgs; return sizeof(TRITON12_STATEOBJECT); }

/* Tier 1.1 incremental growth: ID3D12Device7::AddToStateObject (synchronous
 * on the wire). */
static HRESULT APIENTRY
t12AddToStateObject(D3D12DDI_HDEVICE hDevice,
                    const D3D12DDIARG_ADD_TO_STATE_OBJECT_0072 *pArgs,
                    D3D12DDI_HSTATEOBJECT_0054 h, D3D12DDI_HRTSTATEOBJECT_0054 hRT)
{
    (void)hRT;
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_STATEOBJECT so = t12StateObject(h);
    if (!p || !p->pDev || !so || !pArgs)
        return E_INVALIDARG;
    memset(so, 0, sizeof(*so));
    InitializeSRWLock(&so->lock);
    PTRITON12_STATEOBJECT from = t12StateObject(pArgs->StateObjectToGrowFrom);
    if (!from || !from->pSO)
        return E_INVALIDARG;
    ID3D12Device7 *d7 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(p->pDev, &IID_ID3D12Device7, (void **)&d7)) || !d7)
        return E_NOTIMPL;
    T12_SO_BUILD b;
    HRESULT hr = t12BuildStateObjectDesc(pArgs->NumSubobjects, pArgs->pSubobjects, &b);
    if (SUCCEEDED(hr)) {
        D3D12_STATE_OBJECT_DESC desc;
        desc.Type = (D3D12_STATE_OBJECT_TYPE)pArgs->Type;
        desc.NumSubobjects = b.nsubs;
        desc.pSubobjects = b.subs;
        hr = ID3D12Device7_AddToStateObject(d7, &desc, from->pSO, &IID_ID3D12StateObject,
                                            (void **)&so->pSO);
    }
    UINT nsubs = b.nsubs;
    t12SoBuildFree(&b);
    ID3D12Device7_Release(d7);
    return t12StateObjectFinish(so, hr, "AddToStateObject", nsubs);
}

static void *APIENTRY
t12GetShaderIdentifier(D3D12DDI_HSTATEOBJECT_0054 h, LPCWSTR pName)
{
    PTRITON12_STATEOBJECT so = t12StateObject(h);
    if (!so || !so->pProps || !pName)
        return NULL;
    void *ret = NULL;
    AcquireSRWLockExclusive(&so->lock);
    TRITON12_SOID *e = so->ids;
    while (e && wcscmp(e->name, pName))
        e = e->next;
    if (!e) {
        e = (TRITON12_SOID *)calloc(1, sizeof(*e));
        if (e) {
            e->name = _wcsdup(pName);
            if (!e->name) {
                free(e);
                e = NULL;
            } else {
                e->ok = SUCCEEDED(npt_com_get_shader_identifier(so->pProps, pName, e->id));
                e->next = so->ids;
                so->ids = e;
                if (!e->ok)
                    TR_LOG("12.GetShaderIdentifier(%ls): host has no such export", pName);
            }
        }
    }
    if (e && e->ok)
        ret = e->id;
    ReleaseSRWLockExclusive(&so->lock);
    return ret;
}

static UINT APIENTRY
t12GetShaderStackSize(D3D12DDI_HSTATEOBJECT_0054 h, LPCWSTR pName)
{
    PTRITON12_STATEOBJECT so = t12StateObject(h);
    if (!so || !so->pProps || !pName)
        return 0;
    return (UINT)ID3D12StateObjectProperties_GetShaderStackSize(so->pProps, pName);
}

static UINT APIENTRY
t12GetPipelineStackSize(D3D12DDI_HSTATEOBJECT_0054 h)
{
    PTRITON12_STATEOBJECT so = t12StateObject(h);
    if (!so || !so->pProps)
        return 0;
    return (UINT)ID3D12StateObjectProperties_GetPipelineStackSize(so->pProps);
}

static void APIENTRY
t12SetPipelineStackSize(D3D12DDI_HSTATEOBJECT_0054 h, UINT size)
{
    PTRITON12_STATEOBJECT so = t12StateObject(h);
    if (so && so->pProps)
        ID3D12StateObjectProperties_SetPipelineStackSize(so->pProps, size);
}

static VOID APIENTRY
t12SetPipelineState1(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HSTATEOBJECT_0054 h)
{
    ID3D12GraphicsCommandList4 *l4 = t12List4(hList, "SetPipelineState1");
    PTRITON12_STATEOBJECT so = t12StateObject(h);
    if (l4)
        ID3D12GraphicsCommandList4_SetPipelineState1(l4, so ? so->pSO : NULL);
}

/* The shader tables are app GPU memory holding host shader identifiers (from
 * t12GetShaderIdentifier) and host GPU VAs / descriptor handles: nothing to
 * translate, the desc passes through. */
static VOID APIENTRY
t12DispatchRays(D3D12DDI_HCOMMANDLIST hList, const D3D12DDIARG_DISPATCH_RAYS_0054 *pArgs)
{
    ID3D12GraphicsCommandList4 *l4 = t12List4(hList, "DispatchRays");
    if (l4 && pArgs)
        ID3D12GraphicsCommandList4_DispatchRays(l4, (const D3D12_DISPATCH_RAYS_DESC *)pArgs);
}

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

/* ---------- DXR acceleration structures ----------
 * The DDI acceleration-structure types are field-for-field the API ones
 * (inputs incl. the geometry/instance unions, prebuild info, postbuild
 * descs), so they pass through by cast.  Every address in them -- dest,
 * source, scratch, vertex/index/transform buffers, instance descs and the
 * BLAS pointers inside those instance descs -- is a host GPU VA already
 * (GetGPUVirtualAddress answers come from the host), so nothing needs
 * translating, not even inside GPU memory. */
C_ASSERT(sizeof(D3D12DDI_RAYTRACING_GEOMETRY_DESC_0054) == sizeof(D3D12_RAYTRACING_GEOMETRY_DESC));
C_ASSERT(sizeof(D3D12DDI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_0054) ==
         sizeof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS));
C_ASSERT(sizeof(D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_0054) ==
         sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO));
C_ASSERT(sizeof(D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC_0054) ==
         sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC));
C_ASSERT(sizeof(D3D12DDI_RAYTRACING_INSTANCE_DESC_0054) == sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
C_ASSERT(sizeof(D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054) ==
         sizeof(D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER));

static ID3D12GraphicsCommandList4 *
t12List4(D3D12DDI_HCOMMANDLIST hList, const char *what)
{
    ID3D12GraphicsCommandList4 *l4 = t12ListIface(t12List(hList), T12_LIST4);
    if (!l4) {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            TR_LOG("12.%s: host list has no ID3D12GraphicsCommandList4 -- DROPPED", what);
    }
    return l4;
}

static VOID APIENTRY
t12BuildRaytracingAccelerationStructure(D3D12DDI_HCOMMANDLIST hList,
    const D3D12DDIARG_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_0054 *pArgs)
{
    ID3D12GraphicsCommandList4 *l4 = t12List4(hList, "BuildRaytracingAccelerationStructure");
    if (!l4 || !pArgs)
        return;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d;
    memset(&d, 0, sizeof(d));
    d.DestAccelerationStructureData = pArgs->DestAccelerationStructureData;
    memcpy(&d.Inputs, &pArgs->Inputs, sizeof(d.Inputs));
    d.SourceAccelerationStructureData = pArgs->SourceAccelerationStructureData;
    d.ScratchAccelerationStructureData = pArgs->ScratchAccelerationStructureData;
    TR_LOG_HOT("12.BuildRaytracingAccelerationStructure: type=%d n=%u layout=%d flags=0x%x",
               (int)d.Inputs.Type, d.Inputs.NumDescs, (int)d.Inputs.DescsLayout,
               (unsigned)d.Inputs.Flags);
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(
        l4, &d, pArgs->NumPostbuildInfoDescs,
        (const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *)pArgs->pPostbuildInfoDescs);
}

static VOID APIENTRY
t12EmitRaytracingAccelerationStructurePostbuildInfo(D3D12DDI_HCOMMANDLIST hList,
    const D3D12DDIARG_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_0054 *pArgs)
{
    ID3D12GraphicsCommandList4 *l4 =
        t12List4(hList, "EmitRaytracingAccelerationStructurePostbuildInfo");
    if (!l4 || !pArgs)
        return;
    ID3D12GraphicsCommandList4_EmitRaytracingAccelerationStructurePostbuildInfo(
        l4, (const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *)&pArgs->Desc,
        pArgs->NumSourceAccelerationStructures,
        (const D3D12_GPU_VIRTUAL_ADDRESS *)pArgs->pSourceAccelerationStructureData);
}

static VOID APIENTRY
t12CopyRaytracingAccelerationStructure(D3D12DDI_HCOMMANDLIST hList,
    const D3D12DDIARG_COPY_RAYTRACING_ACCELERATION_STRUCTURE_0054 *pArgs)
{
    ID3D12GraphicsCommandList4 *l4 = t12List4(hList, "CopyRaytracingAccelerationStructure");
    if (!l4 || !pArgs)
        return;
    ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(
        l4, pArgs->DestAccelerationStructureData, pArgs->SourceAccelerationStructureData,
        (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE)pArgs->Mode);
}

static VOID APIENTRY
t12RSSetShadingRate(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_SHADING_RATE_0062 Rate,
                    const D3D12DDI_SHADING_RATE_COMBINER_0062 *pCombiners)
{ (void)hList; (void)Rate; (void)pCombiners; }

static VOID APIENTRY
t12RSSetShadingRateImage(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HRESOURCE h)
{ (void)hList; (void)h; }

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
{
    ID3D12GraphicsCommandList6 *l6 = t12ListIface(t12List(hList), T12_LIST6);
    if (l6)
        ID3D12GraphicsCommandList6_DispatchMesh(l6, x, y, z);
}

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
    t->pfnAtomicCopyBufferRegion = t12AtomicCopyBufferRegion;
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
