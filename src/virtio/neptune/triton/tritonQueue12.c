/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 DDI command queues + fence operations: create, execute and the
 * fence entries all forward to the inner device's queue.  Ordering
 * against the app's fences is not established here but by the
 * monitored-fence gate below.
 */

#include "triton12.h"
#include "triton_log.h"

#include "npt_env.h"

#include <stdlib.h>

#include "virtio/virtio-gpu/wddm_hw.h"   /* VIOGPU_CMD_GATE / VIOGPU_DMA_PRIVATE */

/* Arm a GPU-true gate on the queue's drain fence reaching `value`;
 * returns the KMD gate token via *kmd_token_out (FALSE on failure). */
extern BOOL npt_d3d12_ecl_gate_arm(void *fence_wrapper, UINT64 value,
                                   UINT64 *kmd_token_out);

/* Ring identity + decode position for the tile-mapping edge below.  Declared
 * here for the same reason as the gate above: npt_com.h / npt_ring.h carry
 * the wire protocol's own DirectX type declarations, which collide with the
 * SDK d3d12.h this file needs. */
struct npt_ring;
extern struct npt_ring *npt_com_object_ring_seqno(void *self,
                                                  UINT32 *out_seqno);
extern UINT32 npt_ring_wait_seqno(struct npt_ring *ring, UINT32 seqno);

static VOID APIENTRY t12DestroyCommandQueue(D3D12DDI_HDEVICE hDevice,
                                            D3D12DDI_HCOMMANDQUEUE hQueue);
/* Defined with the tile-mapping entry points at the end of this file. */
static void t12TileMapOrder(PTRITON12_QUEUE q);

static D3D12_COMMAND_LIST_TYPE
t12QueueFlagsToApiType(D3D12DDI_COMMAND_QUEUE_FLAGS Flags)
{
    /* A DIRECT queue carries 3D|COMPUTE|COPY (0x7) -- test 3D FIRST.
     * Checking COMPUTE first made the app's direct queue a compute
     * queue on the inner device, and the recorded clear/draw work then
     * hits the host's compute ring and hangs the GPU.  Copies are legal
     * on a compute queue, so a copy-only workload hides the mistake. */
    if (Flags & D3D12DDI_COMMAND_QUEUE_FLAG_3D)
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (Flags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE)
        return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (Flags & D3D12DDI_COMMAND_QUEUE_FLAG_COPY)
        return D3D12_COMMAND_LIST_TYPE_COPY;
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

static SIZE_T APIENTRY
t12CalcPrivateCommandQueueSize(D3D12DDI_HDEVICE hDevice,
                               const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_QUEUE);
}

static HRESULT APIENTRY
t12CreateCommandQueue(D3D12DDI_HDEVICE hDevice,
                      const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *pArgs)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_QUEUE q = pArgs ? (PTRITON12_QUEUE)pArgs->hDrvCommandQueue.pDrvPrivate : NULL;
    if (!p || !p->pDev || !q)
        return E_INVALIDARG;
    memset(q, 0, sizeof(*q));
    q->pDev = p;

    D3D12_COMMAND_QUEUE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Type = t12QueueFlagsToApiType(pArgs->QueueFlags);
    HRESULT hr = ID3D12Device_CreateCommandQueue(
        p->pDev, &desc, &IID_ID3D12CommandQueue, (void **)&q->pQueue);
    TR_LOG("12.CreateCommandQueue: flags=0x%x -> 0x%08lx",
           (unsigned)pArgs->QueueFlags, (unsigned long)hr);
    if (FAILED(hr))
        return hr;

    /* Per-queue kernel context: the runtime binds the context to
     * hRTCommandQueue and runs its kernel fence ops against it.  The
     * KMD accepts a zero-private-data DX12 virtual context (kmprobe). */
    q->hRTCommandQueue = pArgs->hRTCommandQueue;
    if (p->pUMCallbacks && p->pUMCallbacks->pfnCreateContextVirtualCb) {
        D3DDDICB_CREATECONTEXTVIRTUAL ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.NodeOrdinal = 0;
        ctx.EngineAffinity = 1;
        HRESULT chr = p->pUMCallbacks->pfnCreateContextVirtualCb(
            pArgs->hRTCommandQueue, &ctx);
        TR_LOG("12.CreateCommandQueue: CreateContextVirtualCb -> 0x%08lx "
               "hCtx=%p", (unsigned long)chr, ctx.hContext);
        if (FAILED(chr)) {
            ID3D12CommandQueue_Release(q->pQueue);
            q->pQueue = NULL;
            return chr;
        }
        q->hKMContext = ctx.hContext;
    }

    /* Per-queue drain fence for the monitored-fence gates (see
     * t12ExecuteCommandLists).  A queue whose fences can't be gated would
     * complete every app fence at CPU speed -- fail the create loudly
     * instead. */
    hr = ID3D12Device_CreateFence(p->pDev, 0, D3D12_FENCE_FLAG_NONE,
                                  &IID_ID3D12Fence, (void **)&q->pDrainFence);
    if (FAILED(hr)) {
        TR_LOG("12.CreateCommandQueue: drain-fence create FAILED 0x%08lx",
               (unsigned long)hr);
        t12DestroyCommandQueue(hDevice, pArgs->hDrvCommandQueue);
        return hr;
    }
    q->DrainValue = 0;
    /* Event for the present-fence arm in t12Present.  Not fatal if it fails:
     * the arm is simply skipped and flips fall back to today's ungated
     * behaviour rather than the queue failing to create. */
    q->hPresentArmEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!q->hPresentArmEvent)
        TR_LOG("12.CreateCommandQueue: present-arm event create FAILED %lu "
               "(flips not render-gated on this queue)",
               (unsigned long)GetLastError());
    /* 1 ms slice timer for the present wait's value poll (see triton12.h).
     * Not fatal: without it the wait falls back to the event wake plus the
     * process timer resolution. */
    q->hPresentPaceTimer = CreateWaitableTimerExW(NULL, NULL,
                                                  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                  TIMER_ALL_ACCESS);
    if (!q->hPresentPaceTimer)
        TR_LOG("12.CreateCommandQueue: high-res pace timer create FAILED %lu "
               "(present wait at event/timer-resolution granularity)",
               (unsigned long)GetLastError());

    /* Join the device's queue registry so later ECLs on OTHER queues can order
     * themselves behind this one (t12OrderAgainstSiblings).  Not fatal if the
     * registry is full: the queue simply is not ordered against, which is
     * today's behaviour. */
    q->Slot = TRITON12_MAX_QUEUES;
    if (!p->QueueLockInit)
        return S_OK;   /* device create failed to init the lock; stay unordered */
    EnterCriticalSection(&p->QueueLock);
    if (p->QueueCount < TRITON12_MAX_QUEUES) {
        q->Slot = p->QueueCount;
        p->Queues[p->QueueCount++] = (struct TRITON12_QUEUE *)q;
    }
    LeaveCriticalSection(&p->QueueLock);
    if (q->Slot >= TRITON12_MAX_QUEUES)
        TR_LOG("12.CreateCommandQueue: queue registry full (%u); this queue is "
               "NOT cross-queue ordered", TRITON12_MAX_QUEUES);
    return S_OK;
}

static VOID APIENTRY
t12DestroyCommandQueue(D3D12DDI_HDEVICE hDevice, D3D12DDI_HCOMMANDQUEUE hQueue)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (!q)
        return;

    /* Leave the registry before anything is released: a concurrent ECL on a
     * sibling walks Queues[] and touches pDrainFence.  Compact by moving the
     * tail entry down and fixing its Slot, so slots stay dense and every
     * queue's XQueueSeen[] indices remain valid. */
    if (p && p->QueueLockInit) {
        EnterCriticalSection(&p->QueueLock);
        for (UINT i = 0; i < p->QueueCount; i++) {
            if (p->Queues[i] != (struct TRITON12_QUEUE *)q)
                continue;
            PTRITON12_QUEUE moved = (PTRITON12_QUEUE)p->Queues[p->QueueCount - 1];
            p->Queues[i] = (struct TRITON12_QUEUE *)moved;
            p->Queues[--p->QueueCount] = NULL;
            if (moved && moved != q) {
                moved->Slot = i;
                /* The slot now means a different queue, so every sibling's
                 * high-water mark for it is meaningless -- clear them or a
                 * stale mark would suppress real waits. */
                for (UINT j = 0; j < p->QueueCount; j++) {
                    PTRITON12_QUEUE s = (PTRITON12_QUEUE)p->Queues[j];
                    if (s)
                        s->XQueueSeen[i] = 0;
                }
            }
            break;
        }
        LeaveCriticalSection(&p->QueueLock);
    }

    /* The edge names this queue's ring, which dies with the queue's COM
     * wrapper below; drop it before it can dangle. */
    if (p && p->QueueLockInit && q->pQueue) {
        struct npt_ring *ring = npt_com_object_ring_seqno(q->pQueue, NULL);
        EnterCriticalSection(&p->QueueLock);
        if (p->TileMapRing == ring)
            p->TileMapRing = NULL;
        LeaveCriticalSection(&p->QueueLock);
    }

    if (q->hKMContext && p && p->pUMCallbacks &&
        p->pUMCallbacks->pfnDestroyContextCb) {
        D3DDDICB_DESTROYCONTEXT dc;
        memset(&dc, 0, sizeof(dc));
        dc.hContext = q->hKMContext;
        p->pUMCallbacks->pfnDestroyContextCb(q->hRTCommandQueue, &dc);
        q->hKMContext = NULL;
    }
    if (q->pCopyList) {
        ID3D12GraphicsCommandList_Release(q->pCopyList);
        q->pCopyList = NULL;
    }
    for (UINT ci = 0; ci < TRITON12_COPY_RING; ci++) {
        if (q->pCopyAlloc[ci]) {
            ID3D12CommandAllocator_Release(q->pCopyAlloc[ci]);
            q->pCopyAlloc[ci] = NULL;
        }
    }
    if (q->pDrainFence) {
        ID3D12Fence_Release(q->pDrainFence);
        q->pDrainFence = NULL;
    }
    if (q->pQueue) {
        ID3D12CommandQueue_Release(q->pQueue);
        q->pQueue = NULL;
    }
    if (q->hPresentArmEvent) {
        CloseHandle(q->hPresentArmEvent);
        q->hPresentArmEvent = NULL;
    }
    if (q->hPresentPaceTimer) {
        CloseHandle(q->hPresentPaceTimer);
        q->hPresentPaceTimer = NULL;
    }
}

/* ---------- queue table (D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001) ---------- */

/*
 * Monitored-fence gate, once per ECL batch.  The runtime never calls
 * pfnSignalFence/pfnWaitForFence on this driver: app fences are serviced
 * by dxgkrnl monitored-fence packets on the queue's kernel context,
 * which complete once the context's prior DMA completes.  A context
 * carrying no DMA therefore retires every fence at CPU speed while the
 * GPU work is still running on the host, and the app reads results that
 * do not exist yet.
 *
 * The gate restores the WDDM contract: (1) Signal the per-queue drain
 * fence on the inner queue -- rides the wire ring FIFO-after the ECL, so
 * it completes on the host GPU timeline after the batch; (2) arm a KMD
 * event-ring fence that fires a gate token when the host retires it at
 * that completion; (3) submit a VIOGPU_CMD_GATE{token} DMA packet on the
 * queue's kernel context.  The KMD parks the packet until the token
 * fires, so dxgkrnl's fence packets (and CPU waiters/GetCompletedValue)
 * only observe values the GPU truly reached.
 */
static void
t12QueueGate(PTRITON12_QUEUE q)
{
    if (!q->pDrainFence || !q->hKMContext || !q->pDev ||
        !q->pDev->KTCallbacks.pfnSubmitCommandCb) {
        static LONG miss;
        if (miss < 4) { InterlockedIncrement(&miss);
            TR_LOG("12.QueueGate: UNGATED queue %p (no fence/ctx/cb)", (void *)q); }
        return;
    }

    /* Interlocked: t12Present bumps DrainValue too, on the app's present
     * thread, and a racing plain ++ can mint the same value twice --
     * double-signalled values stay non-decreasing (benign) but the gates
     * behind them stop being one-per-batch. */
    const UINT64 v =
        (UINT64)InterlockedIncrement64((volatile LONG64 *)&q->DrainValue);
    HRESULT hr = ID3D12CommandQueue_Signal(q->pQueue, q->pDrainFence, v);
    if (FAILED(hr)) {
        TR_LOG("12.QueueGate: drain Signal FAILED 0x%08lx v=%llu",
               (unsigned long)hr, (unsigned long long)v);
        return;
    }
    t12PublishDrainVisible(q, v);

    UINT64 kmdToken = 0;
    if (!npt_d3d12_ecl_gate_arm(q->pDrainFence, v, &kmdToken)) {
        static LONG armFail;
        if (armFail < 8) { InterlockedIncrement(&armFail);
            TR_LOG("12.QueueGate: gate arm FAILED v=%llu -- fences will run "
                   "EARLY for this batch", (unsigned long long)v); }
        return;
    }

    /* GATE packet: VIOGPU_COMMAND_HDR + token, carried by value in the
     * submission's private data (the KMD's mirror path; GPU VAs are not
     * CPU-mappable, and the packet needs no DMA content). */
    VIOGPU_DMA_PRIVATE priv;
    memset(&priv, 0, sizeof(priv));
    priv.magic = VIOGPU_DMA_PRIV_MAGIC;
    priv.bodySize = sizeof(VIOGPU_COMMAND_HDR) + sizeof(UINT64);
    VIOGPU_COMMAND_HDR *hdr = (VIOGPU_COMMAND_HDR *)priv.body;
    hdr->type = VIOGPU_CMD_GATE;
    hdr->size = sizeof(UINT64);
    hdr->flags = 0;
    hdr->ring_idx = 0;
    memcpy(priv.body + sizeof(VIOGPU_COMMAND_HDR), &kmdToken, sizeof(UINT64));

    D3DDDICB_SUBMITCOMMAND sub;
    memset(&sub, 0, sizeof(sub));
    /* A zero-length submission never becomes a real packet on the
     * context queue -- dxgkrnl then sees the context idle at the app's
     * queue::Signal and CPU-writes the monitored-fence value instantly,
     * bypassing the gate entirely.  Point Commands at the KMD's shmem
     * window base -- a GPU VA reserved in every process -- with a
     * nominal length: the KMD never dereferences it. */
    sub.Commands = 0x700000000ull;
    sub.CommandLength = 64;
    sub.BroadcastContextCount = 1;
    sub.BroadcastContext[0] = q->hKMContext;
    sub.pPrivateDriverData = &priv;
    sub.PrivateDriverDataSize = sizeof(priv);

    hr = q->pDev->KTCallbacks.pfnSubmitCommandCb(q->pDev->hRTDevice.handle,
                                                 &sub);
    if (FAILED(hr)) {
        static LONG subFail;
        if (subFail < 8) { InterlockedIncrement(&subFail);
            TR_LOG("12.QueueGate: SubmitCommandCb FAILED 0x%08lx -- fences "
                   "will run EARLY for this batch", (unsigned long)hr); }
    } else {
        static LONG gateOk;
        if (gateOk < 8) { InterlockedIncrement(&gateOk);
            TR_LOG("12.QueueGate: GATED v=%llu kmdToken=%llu",
                   (unsigned long long)v, (unsigned long long)kmdToken); }
    }
}

/*
 * Cross-queue submission ordering.
 *
 * An app's ID3D12CommandQueue::Wait(fence, v) never reaches this driver: the
 * runtime services D3D12 fences through dxgkrnl monitored-fence packets and
 * never calls pfnWaitForFence, so t12WaitForFence exists only in case some
 * runtime one day does.  On real hardware that costs nothing, because the GPU
 * work itself travels in kernel DMA packets that the scheduler holds until
 * the wait clears.  Here it does not: t12ExecuteCommandLists forwards the
 * batch to the host over the ring, in user mode, immediately, and dxgkrnl
 * parks only the kernel packets.  The host is left running the app's DIRECT
 * and COMPUTE queues on two independent host queues with no ordering at all,
 * so a consumer pass samples a surface its producer is still writing.
 *
 * Before forwarding a batch on queue Q, therefore, make Q's inner host queue
 * Wait on every sibling queue's drain fence at that sibling's current value.
 * The drain fences already exist and are already signalled once per batch by
 * t12QueueGate, so this adds no objects: it turns "submitted earlier on
 * another queue" into a real ordering edge on the host timeline.  A per-queue
 * high-water mark per sibling keeps it to one wait per sibling per advance.
 *
 * This enforces SUBMISSION order, not the app's waits, since the waits are
 * not observable here at all.  It is correct only when the producer's
 * ExecuteCommandLists precedes the consumer's -- the ordinary pattern.  An
 * app that submits the consumer first and relies on a later Wait still races.
 * Closing that needs execution ordering routed through the kernel the way the
 * gate already routes completion: a DMA packet whose KMD handler releases a
 * host-side start fence, so host execution order follows dxgkrnl's scheduling
 * order.
 *
 * It also serialises async compute against graphics, so the overlap is lost.
 * NPT_DEBUG=no_d3d12_xqueue_order turns it off. */
static BOOL
t12XQueueOrderEnabled(void)
{
    npt_env_init();
    return !NPT_DEBUG(NO_D3D12_XQUEUE_ORDER);
}

static void
t12OrderAgainstSiblings(PTRITON12_QUEUE q)
{
    PTRITON12_DEVICE p = q->pDev;
    if (!p || !p->QueueLockInit || !t12XQueueOrderEnabled())
        return;

    /* Snapshot under the lock, then issue the waits outside it:
     * ID3D12CommandQueue_Wait goes to the host over the wire, and holding a
     * lock across that would serialise every queue in the process on it. */
    ID3D12Fence *fences[TRITON12_MAX_QUEUES];
    UINT64 values[TRITON12_MAX_QUEUES];
    UINT slots[TRITON12_MAX_QUEUES];
    UINT n = 0;

    EnterCriticalSection(&p->QueueLock);
    for (UINT i = 0; i < p->QueueCount; i++) {
        PTRITON12_QUEUE s = (PTRITON12_QUEUE)p->Queues[i];
        if (!s || s == q || !s->pDrainFence)
            continue;
        /* Aligned 64-bit read on x64 is atomic; a stale (lower) value only
         * means a weaker edge this time, never a wrong one.  DrainVisible
         * carries only values whose Signal is already encoded into the
         * wire ring (see t12PublishDrainVisible), so waiting on it cannot
         * deadlock. */
        const UINT64 v = s->DrainVisible;
        if (v == 0 || s->Slot >= TRITON12_MAX_QUEUES ||
            v <= q->XQueueSeen[s->Slot])
            continue;
        q->XQueueSeen[s->Slot] = v;
        fences[n] = s->pDrainFence;
        values[n] = v;
        slots[n]  = s->Slot;
        n++;
    }
    LeaveCriticalSection(&p->QueueLock);

    for (UINT i = 0; i < n; i++) {
        HRESULT hr = ID3D12CommandQueue_Wait(q->pQueue, fences[i], values[i]);
        static LONG s_n;
        LONG ln = InterlockedIncrement(&s_n);
        if (ln <= 8 || (ln & 1023) == 0)
            TR_LOG("12.XQueueOrder #%ld: queue %u waits on queue %u drain "
                   "v=%llu -> 0x%08lx", (long)ln, q->Slot, slots[i],
                   (unsigned long long)values[i], (unsigned long)hr);
    }
}

static VOID APIENTRY
t12ExecuteCommandLists(D3D12DDI_HCOMMANDQUEUE hQueue, UINT Count,
                       const D3D12DDI_HCOMMANDLIST *pCommandLists)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (!q || !q->pQueue || !Count || !pCommandLists)
        return;

    /* Before the batch reaches the host, not after: the wait has to be on the
     * host queue's timeline ahead of this batch's work. */
    t12OrderAgainstSiblings(q);
    /* Likewise for tile mappings this batch may consume. */
    t12TileMapOrder(q);

    ID3D12CommandList *stackLists[8];
    ID3D12CommandList **lists = stackLists;
    if (Count > 8) {
        lists = (ID3D12CommandList **)malloc(Count * sizeof(*lists));
        if (!lists)
            return;
    }
    UINT n = 0;
    for (UINT i = 0; i < Count; i++) {
        PTRITON12_LIST l = (PTRITON12_LIST)pCommandLists[i].pDrvPrivate;
        if (l && l->pList)
            lists[n++] = (ID3D12CommandList *)l->pList;
    }
    TR_LOG_HOT("12.ExecuteCommandLists: %u list(s)", n);
    if (n) {
        ID3D12CommandQueue_ExecuteCommandLists(q->pQueue, n, lists);
        t12QueueGate(q);
    }
    if (lists != stackLists)
        free(lists);
}

static void APIENTRY
t12SignalFence(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDIARG_FENCE_OPERATION *pOp)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    PTRITON12_FENCE f = pOp ? (PTRITON12_FENCE)pOp->Fence.pDrvPrivate : NULL;
    if (!q || !q->pQueue || !f || !f->pFence)
        return;

    /* A signal is what another queue waits on, so it must not reach the
     * host ahead of a tile mapping the waiter's work will consume. */
    t12TileMapOrder(q);

    /* Forward to the inner queue so the host GPU timeline carries the
     * signal.  No CPU wait here: the runtime services app fences through
     * dxgkrnl monitored-fence packets on the queue's kernel context,
     * which the per-ECL gate packets (t12QueueGate) hold to real GPU
     * completion.  (In practice the runtime never calls this DDI on this
     * driver -- kernel packets carry the fence model -- but the forward
     * keeps the host timeline correct if it ever does.) */
    HRESULT hr = ID3D12CommandQueue_Signal(q->pQueue, f->pFence, pOp->Value);
    TR_LOG_HOT("12.SignalFence: v=%llu -> 0x%08lx",
           (unsigned long long)pOp->Value, (unsigned long)hr);
    (void)hr;
}

static void APIENTRY
t12WaitForFence(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDIARG_FENCE_OPERATION *pOp)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    PTRITON12_FENCE f = pOp ? (PTRITON12_FENCE)pOp->Fence.pDrvPrivate : NULL;
    if (!q || !q->pQueue || !f || !f->pFence)
        return;
    HRESULT hr = ID3D12CommandQueue_Wait(q->pQueue, f->pFence, pOp->Value);
    TR_LOG_HOT("12.WaitForFence: v=%llu -> 0x%08lx",
           (unsigned long long)pOp->Value, (unsigned long)hr);
    (void)hr; /* only consumed by TR_LOG_HOT, which compiles out by default */
}

static SIZE_T APIENTRY
t12CalcPrivateCommandQueueSize0023(D3D12DDI_HDEVICE hDevice,
                                   const D3D12DDIARG_CREATECOMMANDQUEUE_0023 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_QUEUE);
}

/* _0023 moved the two queue handles out of the arg struct and added
 * QueueCreationFlags (GLOBAL_REALTIME_PRIORITY), which the host queue
 * does not model. */
static HRESULT APIENTRY
t12CreateCommandQueue0023(D3D12DDI_HDEVICE hDevice,
                          const D3D12DDIARG_CREATECOMMANDQUEUE_0023 *pArgs,
                          D3D12DDI_HCOMMANDQUEUE hDrvCommandQueue,
                          D3D12DDI_HRTCOMMANDQUEUE hRTCommandQueue)
{
    if (!pArgs)
        return E_INVALIDARG;
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 a;
    memset(&a, 0, sizeof(a));
    a.hDrvCommandQueue = hDrvCommandQueue;
    a.hRTCommandQueue  = hRTCommandQueue;
    a.QueueFlags       = pArgs->QueueFlags;
    a.NodeMask         = pArgs->NodeMask;
    return t12CreateCommandQueue(hDevice, &a);
}

static SIZE_T APIENTRY
t12CalcPrivateCommandQueueSize0050(D3D12DDI_HDEVICE hDevice,
                                   const D3D12DDIARG_CREATECOMMANDQUEUE_0050 *pArgs)
{
    (void)hDevice; (void)pArgs;
    return sizeof(TRITON12_QUEUE);
}

/* _0050 adds a scheduling group, which HARDWARE_SCHEDULING_CAPS reports
 * as never used (ComputeQueuesPer3DQueue = 0), so it is always null. */
static HRESULT APIENTRY
t12CreateCommandQueue0050(D3D12DDI_HDEVICE hDevice,
                          const D3D12DDIARG_CREATECOMMANDQUEUE_0050 *pArgs,
                          D3D12DDI_HCOMMANDQUEUE hDrvCommandQueue,
                          D3D12DDI_HRTCOMMANDQUEUE hRTCommandQueue)
{
    if (!pArgs)
        return E_INVALIDARG;
    D3D12DDIARG_CREATECOMMANDQUEUE_0001 a;
    memset(&a, 0, sizeof(a));
    a.hDrvCommandQueue = hDrvCommandQueue;
    a.hRTCommandQueue  = hRTCommandQueue;
    a.QueueFlags       = pArgs->QueueFlags;
    a.NodeMask         = pArgs->NodeMask;
    return t12CreateCommandQueue(hDevice, &a);
}

void
triton12InstallQueueDeviceFuncs0062(D3D12DDI_DEVICE_FUNCS_CORE_0062 *t)
{
    t->pfnCalcPrivateCommandQueueSize = t12CalcPrivateCommandQueueSize0050;
    t->pfnCreateCommandQueue          = t12CreateCommandQueue0050;
}

void
triton12InstallQueueDeviceFuncs0033(D3D12DDI_DEVICE_FUNCS_CORE_0033 *t)
{
    t->pfnCalcPrivateCommandQueueSize = t12CalcPrivateCommandQueueSize0023;
    t->pfnCreateCommandQueue          = t12CreateCommandQueue0023;
}

void
triton12InstallQueueDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnCalcPrivateCommandQueueSize = t12CalcPrivateCommandQueueSize;
    t->pfnCreateCommandQueue          = t12CreateCommandQueue;
    t->pfnDestroyCommandQueue         = t12DestroyCommandQueue;
}

/* Tile mappings.  A reserved resource the host backs sparsely
 * (r->TiledHost, see tritonResource12.c) gets UpdateTileMappings and
 * CopyTileMappings forwarded: the host maps sparse-heap tiles for the
 * named regions and unmapped tiles read zero.  A resource on the
 * committed-backing shim is fully backed at create, so for it these are
 * deliberate no-ops.
 *
 * The DDI structs are layout-identical to the API ones (the SDK's public
 * {X,Y,Z,Subresource} / {NumTiles,UseBox,Width,Height,Depth} PODs and a
 * UINT flag enum), so the arrays pass through by cast. */
C_ASSERT(sizeof(D3D12DDI_TILED_RESOURCE_COORDINATE) == sizeof(D3D12_TILED_RESOURCE_COORDINATE));
C_ASSERT(sizeof(D3D12DDI_TILE_REGION_SIZE) == sizeof(D3D12_TILE_REGION_SIZE));
C_ASSERT(sizeof(D3D12DDI_TILE_RANGE_FLAGS) == sizeof(D3D12_TILE_RANGE_FLAGS));

/* The wire UpdateTileMappings is fire-and-forget and nothing downstream
 * orders it: an app fence placed after it retires on the KMD, which says
 * nothing about the host, and t12OrderAgainstSiblings only edges sibling
 * queues against this queue's ECL drains.  A copy queue's tile fills can
 * therefore reach the host before the map does and land on a tile that is
 * still unmapped (seen as black terrain).
 *
 * The host applies the mapping when the queue's ring decodes the command,
 * and publishes its decode position into that ring's shared memory.  So
 * record where the update sits, and have anything that could consume it
 * from a DIFFERENT ring wait for that position first: the wait is a
 * shared-memory read, and by the time a consumer reaches it the host has
 * almost always decoded past it already.  Work submitted on the mapping
 * queue itself is behind the update in its own ring and needs nothing.
 *
 * Unmaps carry an edge too.  Reading an unmapped tile is undefined, so an
 * unmap racing dependent work needs no ordering of its own -- but a later
 * map of the same tiles from another queue does, and the edge is the only
 * thing that orders it against the unmap. */
static void
t12TileMapPublish(PTRITON12_QUEUE q)
{
    PTRITON12_DEVICE p = q->pDev;
    UINT32 seqno = 0;
    struct npt_ring *ring = npt_com_object_ring_seqno(q->pQueue, &seqno);
    if (!p || !p->QueueLockInit || !ring)
        return;
    EnterCriticalSection(&p->QueueLock);
    p->TileMapRing = ring;
    p->TileMapSeqno = seqno;
    LeaveCriticalSection(&p->QueueLock);
}

static void
t12TileMapOrder(PTRITON12_QUEUE q)
{
    PTRITON12_DEVICE p = q->pDev;
    if (!p || !p->QueueLockInit)
        return;
    EnterCriticalSection(&p->QueueLock);
    struct npt_ring *edge = p->TileMapRing;
    const UINT32 seqno = p->TileMapSeqno;
    LeaveCriticalSection(&p->QueueLock);
    if (!edge || edge == npt_com_object_ring_seqno(q->pQueue, NULL))
        return;
    npt_ring_wait_seqno(edge, seqno);
}

static VOID APIENTRY
t12UpdateTileMappings(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDI_HRESOURCE hRes,
                      UINT NumRegions,
                      const D3D12DDI_TILED_RESOURCE_COORDINATE *pCoords,
                      const D3D12DDI_TILE_REGION_SIZE *pSizes,
                      D3D12DDI_HHEAP hPool, UINT NumRanges,
                      const D3D12DDI_TILE_RANGE_FLAGS *pRangeFlags,
                      const UINT *pHeapStartOffsets,
                      const UINT *pRangeTileCounts,
                      D3D12DDI_TILE_MAPPING_FLAGS Flags)
{
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hRes.pDrvPrivate;
    PTRITON12_HEAP h = (PTRITON12_HEAP)hPool.pDrvPrivate;
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (r && r->TiledHost && r->pResource && q && q->pQueue) {
        static LONG onceF;
        if (!InterlockedExchange(&onceF, 1))
            TR_LOG("12.UpdateTileMappings: forwarding to the host (sparse "
                   "backing; regions=%u ranges=%u)", NumRegions, NumRanges);
        /* Against a pending edge from another queue, so mappings apply in
         * the order the app issued them. */
        t12TileMapOrder(q);
        ID3D12CommandQueue_UpdateTileMappings(
            q->pQueue, r->pResource, NumRegions,
            (const D3D12_TILED_RESOURCE_COORDINATE *)pCoords,
            (const D3D12_TILE_REGION_SIZE *)pSizes,
            (h && h->pHeap) ? h->pHeap : NULL, NumRanges,
            (const D3D12_TILE_RANGE_FLAGS *)pRangeFlags,
            pHeapStartOffsets, pRangeTileCounts,
            (D3D12_TILE_MAPPING_FLAGS)Flags);
        t12TileMapPublish(q);
        return;
    }
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        TR_LOG("12.UpdateTileMappings: no-op (committed-backing shim; "
               "regions=%u ranges=%u)", NumRegions, NumRanges);
}

static VOID APIENTRY
t12CopyTileMappings(D3D12DDI_HCOMMANDQUEUE hQueue, D3D12DDI_HRESOURCE hDst,
                    const D3D12DDI_TILED_RESOURCE_COORDINATE *pDstCoord,
                    D3D12DDI_HRESOURCE hSrc,
                    const D3D12DDI_TILED_RESOURCE_COORDINATE *pSrcCoord,
                    const D3D12DDI_TILE_REGION_SIZE *pSize,
                    D3D12DDI_TILE_MAPPING_FLAGS Flags)
{
    PTRITON12_RESOURCE d = (PTRITON12_RESOURCE)hDst.pDrvPrivate;
    PTRITON12_RESOURCE s = (PTRITON12_RESOURCE)hSrc.pDrvPrivate;
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    if (d && d->TiledHost && s && s->TiledHost && q && q->pQueue) {
        static LONG onceF;
        if (!InterlockedExchange(&onceF, 1))
            TR_LOG("12.CopyTileMappings: forwarding to the host (sparse backing)");
        t12TileMapOrder(q);
        ID3D12CommandQueue_CopyTileMappings(
            q->pQueue, d->pResource,
            (const D3D12_TILED_RESOURCE_COORDINATE *)pDstCoord, s->pResource,
            (const D3D12_TILED_RESOURCE_COORDINATE *)pSrcCoord,
            (const D3D12_TILE_REGION_SIZE *)pSize,
            (D3D12_TILE_MAPPING_FLAGS)Flags);
        t12TileMapPublish(q);
        return;
    }
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        TR_LOG("12.CopyTileMappings: no-op (committed-backing shim)");
}

void
triton12InstallQueueFuncs(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *t)
{
    t->pfnExecuteCommandLists = t12ExecuteCommandLists;
    t->pfnSignalFence         = t12SignalFence;
    t->pfnWaitForFence        = t12WaitForFence;
    t->pfnUpdateTileMappings  = t12UpdateTileMappings;
    t->pfnCopyTileMappings    = t12CopyTileMappings;
}
