/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 present path.  The runtime performs the kernel
 * present itself; our list-table pfnPresent fills D3D12DDI_PRESENT_0003
 * with the backbuffer's shared-blob KM allocation and the presenting
 * queue's kernel context.  Presentable committed resources (PRIMARY
 * heap flag) get their KM allocation via the same shared-bridge blob
 * export + pfnAllocateCb flow as Triton D3D11 primaries
 * (tritonKmFillSharedExchange); the host exports its resources through
 * ID3D12Device::CreateSharedHandle natively.
 */

#include "triton12.h"
#include "tritonSharedBridge.h"
#include "npt_shared_texture.h"
#include "virtio/virtio-gpu/wddm_hw.h"   /* VIOGPU escape / allocation ABI */
#include "triton_log.h"

#include "npt_env.h"
#include "npt_common.h"   /* npt_log: stderr mirrors for rare present-path
                           * failures, so launcher-spawned games (no ODS
                           * listener) still leave evidence */
#include "tritonPresentTiming.h"

void
triton12DeallocateKM(PTRITON12_DEVICE p, D3DKMT_HANDLE *phAllocation)
{
    if (!p || !phAllocation || !*phAllocation)
        return;
    if (p->pUMCallbacks && p->pUMCallbacks->pfnDeallocateCb) {
        D3D12DDICB_DEALLOCATE_0022 da;
        memset(&da, 0, sizeof(da));
        da.NumAllocations = 1;
        da.HandleList     = phAllocation;
        HRESULT hr = p->pUMCallbacks->pfnDeallocateCb(p->hRTDevice, &da);
        if (FAILED(hr))
            TR_LOG("12.dealloc: pfnDeallocateCb(alloc=0x%x) failed 0x%08lx",
                   *phAllocation, (unsigned long)hr);
    }
    *phAllocation = 0;
}

BOOL
triton12RegisterSharedBlob(PTRITON12_DEVICE p, PTRITON12_RESOURCE r,
                           BOOL primary, ID3D12Resource *pSurf)
{
    /* D3D12 leaves the legacy D3DDDI_DEVICECALLBACKS.pfnAllocateCb NULL;
     * allocation rides the corelayer callback instead. */
    if (!pSurf)
        pSurf = r->pResource;
    if (!p->pUMCallbacks || !p->pUMCallbacks->pfnAllocateCb || !pSurf)
        return FALSE;

    tritonKmEnsureRuntimeCtx(&p->Km);

    TRITON_KM_SHARED_DESC d;
    memset(&d, 0, sizeof(d));
    d.Width          = r->Desc.Width;
    d.Height         = r->Desc.Height;
    d.MipLevels      = r->Desc.MipLevels;
    d.ArraySize      = r->Desc.DepthOrArraySize;
    d.Format         = r->Desc.Format;
    /* A companion is single-sample whatever the frame it carries. */
    d.SampleCount    = pSurf == r->pResource ? r->Desc.SampleDesc.Count : 1;
    d.Is2D           = r->Desc.ResourceType == D3D12DDI_RT_TEXTURE2D;
    d.Usage          = D3D11_USAGE_DEFAULT;
    d.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.CpuAccessFlags = 0;
    d.MiscFlags      = D3D11_RESOURCE_MISC_SHARED;

    VIOGPU_CREATE_ALLOCATION_EXCHANGE ax;
    if (!tritonKmFillSharedExchange(&ax, pSurf, &d, primary))
        return FALSE;

    D3D12DDI_ALLOCATION_INFO_0022 ai;
    memset(&ai, 0, sizeof(ai));
    ai.pPrivateDriverData    = &ax;
    ai.PrivateDriverDataSize = sizeof(ax);
    if (primary) {
        ai.Flags = D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY;
        ai.VidPnSourceId = 0;
    }

    D3D12DDICB_ALLOCATE_0022 cb;
    memset(&cb, 0, sizeof(cb));
    cb.hResource        = r->hRTResource.handle;
    cb.NumAllocations   = 1;
    cb.pAllocationInfo  = &ai;

    HRESULT hr = p->pUMCallbacks->pfnAllocateCb(p->hRTDevice, &cb);
    if (FAILED(hr) || !ai.hAllocation) {
        TR_LOG("12.shared: pfnAllocateCb failed hr=0x%08lx",
               (unsigned long)hr);
        return FALSE;
    }
    /* A residency-only placeholder being replaced by the real export. */
    triton12DeallocateKM(p, &r->hKMAllocation);
    r->hKMAllocation = ai.hAllocation;
    tritonKmRequestResidency(&p->Km, ai.hAllocation);
    TR_LOG_HOT("12.shared: exporter blob_id=0x%llx alloc=0x%x %llux%u primary=%d",
           (unsigned long long)ax.OptionsShared.blob_id, ai.hAllocation,
           (unsigned long long)r->Desc.Width, r->Desc.Height, primary);
    return TRUE;
}

/* ---------- residency-only KM allocation ---------- */

/* Unique-per-live-allocation cookie (wddm_hw.h): pid<<32 | counter keeps
 * bits 62/63 clear for the KMD's namespaces; bit 31 of the counter word
 * separates this minter from the transport's (npt_mint_alloc_cookie) in
 * the same process. */
static ULONGLONG
t12MintAllocCookie(void)
{
    static volatile LONG counter;
    return ((ULONGLONG)GetCurrentProcessId() << 32) | 0x80000000ull |
           ((ULONG)InterlockedIncrement(&counter) & 0x7fffffffu);
}

D3DKMT_HANDLE
triton12AllocResidencyOnly(PTRITON12_DEVICE p, HANDLE hRTResource)
{
    if (!p || !hRTResource)
        return 0;
    if (!p->pUMCallbacks || !p->pUMCallbacks->pfnAllocateCb)
        return 0;

    tritonKmEnsureRuntimeCtx(&p->Km);

    /* The EX form carries a lookup cookie, which is the only way the KMD
     * pairs this allocation's in-create open with the right object: a
     * plain exchange falls back to its KMT-handle map, and dxgkrnl reuses
     * handle values while a deferred-release allocation still owns the
     * old binding, so the open lands on the wrong, still-live allocation
     * and its eventual teardown frees the device-allocation dxgkrnl holds
     * for this one (bugcheck 0x3B in VioGpu3DCloseAllocation).  Thousands
     * of these come and go per frame, which is what makes the reuse
     * window real. */
    VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX ax;
    memset(&ax, 0, sizeof(ax));
    ax.Base.Type = VIOGPU_RESOURCE_TYPE_BLOB;
    /* RESIDENCY_ONLY keeps the KMD from minting a res_id or creating a host
     * resource for this allocation; one page of guest address space is the
     * whole cost, and nothing ever renders with it. */
    ax.Base.OptionsBlob.blob_mem   = VIOGPU_BLOB_MEM_GUEST;
    ax.Base.OptionsBlob.blob_flags = VIOGPU_BLOB_FLAG_RESIDENCY_ONLY;
    ax.Base.OptionsBlob.blob_id    = 0;
    ax.Base.Size                   = 4096;
    ax.LookupCookie                = t12MintAllocCookie();

    D3D12DDI_ALLOCATION_INFO_0022 ai;
    memset(&ai, 0, sizeof(ai));
    ai.pPrivateDriverData    = &ax;
    ai.PrivateDriverDataSize = sizeof(ax);

    D3D12DDICB_ALLOCATE_0022 cb;
    memset(&cb, 0, sizeof(cb));
    cb.hResource       = hRTResource;
    cb.NumAllocations  = 1;
    cb.pAllocationInfo = &ai;

    HRESULT hr = p->pUMCallbacks->pfnAllocateCb(p->hRTDevice, &cb);
    if (FAILED(hr) || !ai.hAllocation) {
        TR_LOG("12.residency: pfnAllocateCb failed hr=0x%08lx",
               (unsigned long)hr);
        return 0;
    }
    return ai.hAllocation;
}

BOOL
triton12RegisterResidencyAlloc(PTRITON12_DEVICE p, PTRITON12_RESOURCE r)
{
    if (!p || !r || r->hKMAllocation)
        return FALSE;
    r->hKMAllocation = triton12AllocResidencyOnly(p, r->hRTResource.handle);
    if (!r->hKMAllocation)
        return FALSE;
    r->ResidencyOnlyAlloc = TRUE;
    return TRUE;
}

/* ---------- lazy KM allocation on demand ----------
 *
 * No DDI flag marks swapchain backbuffers at create time (they arrive
 * as plain RT textures, heapFlags=0x20 resFlags=0x11).  The runtime
 * asks the driver for a resource's kernel allocation through
 * pfnCheckResourceAllocationHandle when it needs one (CreateSharedHandle
 * for the DWM bind) -- create the shared-blob allocation lazily there,
 * so ordinary resources stay host-only. */

static D3DKMT_HANDLE APIENTRY
t12CheckResourceAllocationHandle(D3D12DDI_HDEVICE hDevice,
                                 D3D10DDI_HRESOURCE hResource)
{
    PTRITON12_DEVICE p = triton12Device(hDevice);
    PTRITON12_RESOURCE r = (PTRITON12_RESOURCE)hResource.pDrvPrivate;
    if (!p || !r)
        return 0;
    /* Replace a residency-only placeholder with a real export. */
    if (!r->hKMAllocation || r->ResidencyOnlyAlloc) {
        if (triton12RegisterSharedBlob(p, r, FALSE, NULL))
            r->ResidencyOnlyAlloc = FALSE;
    }
    /* A placeholder that survived the export retry names no memory: report
     * no allocation, so a swapchain create fails cleanly instead of
     * presenting it. */
    if (r->ResidencyOnlyAlloc)
        return 0;
    TR_LOG_HOT("12.CheckResourceAllocationHandle: r=%p -> 0x%x", (void *)r,
           r->hKMAllocation);
    return r->hKMAllocation;
}

static UINT APIENTRY
t12GetPresentPrivateDriverDataSize(D3D12DDI_HDEVICE hDevice,
                                   const D3D12DDIARG_PRESENT_0001 *pArgs)
{
    /* No per-present private driver data; the KMD present path reads
     * everything it needs from the allocation. */
    (void)hDevice; (void)pArgs;
    return 0;
}

void
triton12InstallPresentDeviceFuncs(D3D12DDI_DEVICE_FUNCS_CORE_0022 *t)
{
    t->pfnCheckResourceAllocationHandle = t12CheckResourceAllocationHandle;
    t->pfnGetPresentPrivateDriverDataSize = t12GetPresentPrivateDriverDataSize;
}

/* ---------- list-table pfnPresent ---------- */

/* Present-stage timing (NPT_DEBUG=present_timing), reported every
 * TRITON12_PT_REPORT presents: sig (Signal + completed-value read), arm
 * (SetEventOnCompletion, including the SUBMIT_PRESENT_FENCE escape its
 * override issues), wait (the value poll), plus how many presents found the
 * value already complete, how many the KMD gated, how many hit the deadline,
 * and the largest single wait.  The clock and the enable are shared with the
 * D3D11 path in tritonPresentTiming.h. */
#define TRITON12_PT_REPORT 256

static struct {
    LONG64 n, already, gated, timeouts, sig_us, arm_us, wait_us, wait_max_us;
} g_pt12;

static void
t12PtAccount(double sig, double arm, double wait, BOOL already, BOOL gated,
             BOOL timedOut)
{
    if (!NPT_DEBUG(PRESENT_TIMING))
        return;
    InterlockedAdd64(&g_pt12.sig_us, (LONG64)sig);
    InterlockedAdd64(&g_pt12.arm_us, (LONG64)arm);
    InterlockedAdd64(&g_pt12.wait_us, (LONG64)wait);
    if (already)
        InterlockedIncrement64(&g_pt12.already);
    if (gated)
        InterlockedIncrement64(&g_pt12.gated);
    if (timedOut)
        InterlockedIncrement64(&g_pt12.timeouts);
    triton_pt_max(&g_pt12.wait_max_us, (LONG64)wait);
    LONG64 n = InterlockedIncrement64(&g_pt12.n);
    if (n % TRITON12_PT_REPORT == 0) {
        LONG64 sigT  = InterlockedExchange64(&g_pt12.sig_us, 0);
        LONG64 armT  = InterlockedExchange64(&g_pt12.arm_us, 0);
        LONG64 waitT = InterlockedExchange64(&g_pt12.wait_us, 0);
        LONG64 alr   = InterlockedExchange64(&g_pt12.already, 0);
        LONG64 gat   = InterlockedExchange64(&g_pt12.gated, 0);
        LONG64 to    = InterlockedExchange64(&g_pt12.timeouts, 0);
        LONG64 wmax  = InterlockedExchange64(&g_pt12.wait_max_us, 0);
        TR_LOG("12.PT n=%lld avg_us sig=%lld arm=%lld wait=%lld "
               "wait_max=%lld already=%lld gated=%lld timeouts=%lld /%d",
               n, sigT / TRITON12_PT_REPORT, armT / TRITON12_PT_REPORT,
               waitT / TRITON12_PT_REPORT, wmax, alr, gat, to,
               TRITON12_PT_REPORT);
    }
}

/* Wait until the drain fence reaches v, polling its completed value on the
 * queue's high-resolution 1 ms slices (Sleep(1) quantizes to the process
 * timer resolution).  No completion event is registered: the
 * SUBMIT_PRESENT_FENCE escape behind SetEventOnCompletion stamps this
 * thread's present token, which only the flip arm in t12Present may do.
 * Returns FALSE when the deadline passes first. */
static BOOL
t12PollDrain(PTRITON12_QUEUE q, UINT64 v, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (ID3D12Fence_GetCompletedValue(q->pDrainFence) < v) {
        if (GetTickCount64() >= deadline)
            return FALSE;
        LARGE_INTEGER due;
        due.QuadPart = -10000;   /* 1 ms, relative */
        if (q->hPresentPaceTimer &&
            SetWaitableTimer(q->hPresentPaceTimer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(q->hPresentPaceTimer, 5);
        else
            Sleep(1);
    }
    return TRUE;
}

/* Fill src's companion from its frame.  A single-sample source is copied
 * under implicit promotion (see t12PresentCompanionCopy).  A multisampled
 * source is resolved: the companion is always single-sample, and the
 * resolve states are not promotable, so both resources are transitioned
 * explicitly and returned to PRESENT (COMMON), where the copy path also
 * leaves the source. */
static void
t12RecordCompanionFill(ID3D12GraphicsCommandList *list,
                       PTRITON12_RESOURCE src)
{
    if (src->Desc.SampleDesc.Count <= 1) {
        ID3D12GraphicsCommandList_CopyResource(list, src->pCompanion,
                                               src->pResource);
        return;
    }
    D3D12_RESOURCE_BARRIER b[2];
    memset(b, 0, sizeof(b));
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = src->pResource;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    b[1] = b[0];
    b[1].Transition.pResource = src->pCompanion;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, b);
    ID3D12GraphicsCommandList_ResolveSubresource(
        list, src->pCompanion, 0, src->pResource, 0,
        (DXGI_FORMAT)src->Desc.Format);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, b);
}

/* ---------- companion present copy ----------
 *
 * Companion-backed back buffers (tritonResource12.c) render into a NATIVE
 * host texture; the KM allocation the runtime's kernel present names is the
 * shared linear companion.  Copy the finished frame into it here, on the
 * presenting queue, BEFORE the present's GPU-done gate -- the drain-fence
 * arm for a flip, the queue gate for a blt -- whose signal is then queued
 * behind this copy, so the waits and KMD packet gates that key on it stay
 * GPU-true for the copy too: the compositor must never sample a mid-copy
 * companion.
 *
 * A flip present cannot use the DDI's own AddedGpuWork/hCommandList
 * mechanism for this: the runtime executes that list only after
 * pfnPresent returns, which is after the arm -- the copy would land
 * outside the gate.  A blt present does use it (t12Present); its kernel
 * packet is ordered by the queue gate the list's ExecuteCommandLists
 * submits instead.
 *
 * The copy relies on implicit state promotion with no barriers: the source
 * sits in PRESENT (== COMMON), is promoted to COPY_SOURCE and decays back
 * after the copy; the companion is promoted to COPY_DEST by its first copy
 * and stays there.  present-copy-test in d3dmetal-native verifies
 * the host accepts exactly this shape (native optimally-tiled source,
 * substituted linear dest, no barriers).
 *
 * Failure policy: any failure skips the copy for this frame -- the
 * compositor shows the previous frame's companion contents, which beats
 * tearing down the present.  A rig build failure latches CopyRigDead. */
static void
t12PresentCompanionCopy(PTRITON12_QUEUE q, PTRITON12_RESOURCE src)
{
    PTRITON12_DEVICE p = q->pDev;
    UINT i;
    if (q->CopyRigDead || !p || !p->pDev)
        return;

    if (!q->pCopyList) {
        for (i = 0; i < TRITON12_COPY_RING; i++) {
            if (FAILED(ID3D12Device_CreateCommandAllocator(
                    p->pDev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    &IID_ID3D12CommandAllocator,
                    (void **)&q->pCopyAlloc[i])))
                break;
        }
        if (i == TRITON12_COPY_RING &&
            SUCCEEDED(ID3D12Device_CreateCommandList(
                p->pDev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                q->pCopyAlloc[0], NULL, &IID_ID3D12GraphicsCommandList,
                (void **)&q->pCopyList)))
            ID3D12GraphicsCommandList_Close(q->pCopyList);
        if (!q->pCopyList) {
            for (i = 0; i < TRITON12_COPY_RING; i++) {
                if (q->pCopyAlloc[i]) {
                    ID3D12CommandAllocator_Release(q->pCopyAlloc[i]);
                    q->pCopyAlloc[i] = NULL;
                }
            }
            q->CopyRigDead = TRUE;
            TR_LOG("12.CompanionCopy: rig build FAILED; presents on this "
                   "queue will show stale companions");
            npt_log("companion-copy rig build FAILED (queue %p)", (void *)q);
            return;
        }
        TR_LOG("12.CompanionCopy: rig built (ring=%d)", TRITON12_COPY_RING);
    }

    /* Reuse gate: the drain value stamped at this allocator's last use
     * must have completed, or its list is still in flight and Reset would
     * pull the commands out from under the GPU.  With a ring of 4 and the
     * present pacing depth this is virtually never waited on. */
    const UINT idx = q->CopyIdx++ % TRITON12_COPY_RING;
    if (q->pDrainFence && q->CopyAllocValue[idx] &&
        !t12PollDrain(q, q->CopyAllocValue[idx], 1000)) {
        TR_LOG("12.CompanionCopy: allocator %u still in flight "
               "after 1s; skipping copy this frame", idx);
        npt_log("companion-copy allocator %u STUCK 1s (v=%llu done=%llu)",
                idx, (unsigned long long)q->CopyAllocValue[idx],
                (unsigned long long)
                    ID3D12Fence_GetCompletedValue(q->pDrainFence));
        return;
    }

    if (FAILED(ID3D12CommandAllocator_Reset(q->pCopyAlloc[idx])) ||
        FAILED(ID3D12GraphicsCommandList_Reset(q->pCopyList,
                                               q->pCopyAlloc[idx], NULL))) {
        TR_LOG("12.CompanionCopy: list reset FAILED; skipping copy");
        npt_log("companion-copy list reset FAILED");
        return;
    }
    t12RecordCompanionFill(q->pCopyList, src);
    if (FAILED(ID3D12GraphicsCommandList_Close(q->pCopyList)))
        return;
    {
        ID3D12CommandList *lists[1] = { (ID3D12CommandList *)q->pCopyList };
        ID3D12CommandQueue_ExecuteCommandLists(q->pQueue, 1, lists);
    }
    /* Stamp the allocator with its own drain value so the reuse gate above
     * has something to key on.  This signal also floors the present arm's
     * value: the arm increments past it, keeping values monotonic. */
    if (q->pDrainFence) {
        UINT64 v;
        if (SUCCEEDED(t12DrainSignal(q, &v)))
            q->CopyAllocValue[idx] = v;
    }
}

static VOID APIENTRY
t12Present(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HCOMMANDQUEUE hQueue,
           const D3D12DDIARG_PRESENT_0001 *pArgs, D3D12DDI_PRESENT_0003 *pOut)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    PTRITON12_LIST l = (PTRITON12_LIST)hList.pDrvPrivate;
    if (!pOut)
        return;
    memset(pOut, 0, sizeof(*pOut));
    if (!pArgs || !q)
        return;

    PTRITON12_RESOURCE src = NULL;
    if (pArgs->SurfacesToPresent && pArgs->phSurfacesToPresent)
        src = (PTRITON12_RESOURCE)
            pArgs->phSurfacesToPresent[0].hSurface.pDrvPrivate;
    PTRITON12_RESOURCE dst =
        (PTRITON12_RESOURCE)pArgs->hDstResource.pDrvPrivate;

    /* A source still holding its residency-only placeholder names no memory
     * for the kernel present to copy from.  D3D9On12 presents plain textures
     * (no RENDER_TARGET flag, e.g. its A2R10G10B10 back buffer), which the
     * create path gives no companion; build one here, once.  The companion
     * takes the same shape rule as the create path's -- one plane -- and is
     * single-sample: a multisampled source (a D3D9 MSAA back buffer) is
     * resolved into it, so its format must support the resolve.  A
     * ROW_MAJOR source is left alone: CopyResource needs
     * both resources the same size, which a linear source and an opaque
     * companion need not be.  A shape the rule rejects, or a create the
     * host refuses, is latched so it costs one attempt rather than a host
     * create per frame; any other failure (host memory, the blob export)
     * is retried at the next present. */
    if (src && src->pResource && !src->pCompanion && !src->CompanionFailed &&
        q->pDev && (src->ResidencyOnlyAlloc || !src->hKMAllocation)) {
        D3D12_RESOURCE_DESC desc;
        ID3D12Resource_GetDesc(src->pResource, &desc);
        HRESULT hr = E_INVALIDARG;
        BOOL resolvable = desc.SampleDesc.Count <= 1;
        if (!resolvable) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
            memset(&fs, 0, sizeof(fs));
            fs.Format = desc.Format;
            resolvable = SUCCEEDED(ID3D12Device_CheckFeatureSupport(
                             q->pDev->pDev, D3D12_FEATURE_FORMAT_SUPPORT, &fs,
                             sizeof(fs))) &&
                         (fs.Support1 &
                          D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE);
        }
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc.MipLevels <= 1 && desc.DepthOrArraySize <= 1 &&
            resolvable && desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) {
            D3D12_HEAP_PROPERTIES props;
            memset(&props, 0, sizeof(props));
            props.Type = D3D12_HEAP_TYPE_DEFAULT;
            hr = triton12CreateCompanion(q->pDev, src, &props, &desc);
        }
        if (SUCCEEDED(hr)) {
            src->ResidencyOnlyAlloc = FALSE;
        } else if (hr == E_INVALIDARG || hr == E_NOTIMPL) {
            src->CompanionFailed = TRUE;
            npt_log("present: no companion for source %llux%u fmt=%d "
                    "dim=%d mips=%u array=%u samples=%u layout=%d; it "
                    "presents nothing", (unsigned long long)desc.Width,
                    desc.Height, (int)desc.Format, (int)desc.Dimension,
                    (unsigned)desc.MipLevels, (unsigned)desc.DepthOrArraySize,
                    (unsigned)desc.SampleDesc.Count, (int)desc.Layout);
        }
    }

    pOut->hSrcAllocation = src ? src->hKMAllocation : 0;
    pOut->hDstAllocation = dst ? dst->hKMAllocation : 0;
    pOut->hContext       = q->hKMContext;
    pOut->AddedGpuWork   = FALSE;
    pOut->BackBufferMultiplicity = 1;

    static LONG s_presentN;
    LONG n = InterlockedIncrement(&s_presentN);

    /* Companion-backed back buffer: copy the rendered native frame into
     * the shared companion the KM allocation names, before the arm below
     * so the GPU-done gate covers the copy.  A blt present instead records
     * the copy at the end of the open list the runtime executes ahead of
     * the kernel present: a downlevel presenter (D3D9On12, D3D11On12)
     * records the frame's last writes there, so a copy issued now would
     * read the previous frame.  This KMD claims neither DirectFlip nor
     * independent flip, so the runtime delivers every D3D9On12, D3D11On12
     * and native D3D12 present as a blt, windowed or fullscreen, and no
     * presenter reaches the flip copy below.  A flip whose open list
     * carries the frame's writes (a D3D11On12 swapchain records its clear,
     * update and draw there) would show the previous frame. */
    const BOOL blt = pArgs->Flags.Blt ? TRUE : FALSE;
    if (src && src->pCompanion && src->pResource) {
        if (blt && l && l->pList && !l->AwaitingReset) {
            t12RecordCompanionFill(l->pList, src);
            pOut->AddedGpuWork = TRUE;
        } else if (q->pQueue) {
            t12PresentCompanionCopy(q, src);
            /* A blt present's copy went to the host queue directly, past
             * the gate ExecuteCommandLists would have submitted behind it;
             * submit that gate here so the kernel present waits for it.
             * Without a gate nothing parks the kernel present, so wait on
             * the CPU for the drain value already signalled behind the copy
             * instead. */
            if (blt && !triton12QueueGate(q) && q->pDrainFence &&
                !t12PollDrain(q, t12Load64(&q->DrainVisible), 1000))
                npt_log("present: blt copy not done after 1s (v=%llu "
                        "done=%llu); presenting anyway",
                        (unsigned long long)t12Load64(&q->DrainVisible),
                        (unsigned long long)
                            ID3D12Fence_GetCompletedValue(q->pDrainFence));
        }
    }

    /* A blt present is not gated here.  Its kernel packet never consumes
     * the thread token the arm stamps (the KMD parks only flip packets on
     * it), so arming would leave a stale stamp on this thread for a later
     * flip to consume, and the frame's last writes sit in the open list
     * the runtime executes between this DDI and the kernel present, after
     * any arm issued now.  Its order comes from the queue gate instead:
     * that ExecuteCommandLists submits a gate packet parked until the GPU
     * has retired the list, and the KMD runs one DMA packet at a time, so
     * the blt packet's host copy cannot start before the frame is done.
     *
     * Arm the render->flip token for this frame, then wait for it.
     *
     * ARM.  Without this the KMD's TakeThreadToken() finds no stamp for the
     * flip that follows, gates it on token 0 -- which always reads retired --
     * and promotes the scanout immediately.  The D3D11 path arms the same
     * token (tritonPresentFlushAndGate in tritonDxgi.c).
     *
     * Pairing is per-thread: the runtime calls pfnPresentCb on THIS thread
     * as soon as this DDI returns and DxgkDdiPresent runs synchronously in
     * it, so the arm issued here is the stamp that flip consumes (see the
     * token block in viogpu_adapter.h).
     *
     * WAIT.  Arming alone is not enough for a windowed title, because it is
     * DWM-composited rather than direct-scanout: every scanout carries the
     * desktop primary, never the workload's own surface.  The consumer of
     * this backbuffer is dwm.exe sampling it as a shared texture from another
     * process, and the frame-ready signal it acts on is pfnPresentCb itself,
     * so no scanout-side gate can order that handoff.  The producer must not
     * report the frame until the GPU has finished it -- the same conclusion
     * the D3D11 path reached for composited surfaces.
     *
     * Completion is judged solely by GetCompletedValue -- a per-fence
     * feedback-slot read the host publishes on a ~100 us cadence, the same
     * contract as the D3D11 gate in tritonPresentFlushAndGate.  The KMD
     * completion event is only a wake hint: its delivery chain (host SEOC ->
     * eventfd -> sync worker -> virtio -> DPC -> waiter pool -> SetEvent)
     * carries a multi-ms floor, which polling the value on 1 ms slices
     * undercuts.  The wait is bounded so a lost completion degrades to a
     * late frame rather than a hung present. */
    if (!blt && q->pQueue && q->pDrainFence && q->hPresentArmEvent) {
        UINT64 v;
        const double t0 = TRITON_PT_NOW();
        HRESULT shr = t12DrainSignal(q, &v);
        HRESULT ehr = shr;
        UINT64 done = SUCCEEDED(shr)
            ? ID3D12Fence_GetCompletedValue(q->pDrainFence) : v;
        double t1 = TRITON_PT_NOW();
        double t2 = t1;
        BOOL already = TRUE, gated = FALSE, timedOut = FALSE;
        if (SUCCEEDED(shr) && done < v) {
            already = FALSE;
            /* Drain a stale fire first: waits below exit on the VALUE with
             * their one-shot registration still outstanding, and that late
             * fire leaves this auto-reset event signalled for someone else's
             * frame -- the exact pooled-event bug the D3D11 gate hit.  The
             * event is only armed/waited from this queue's present, so a
             * 0-timeout drain is safe. */
            WaitForSingleObject(q->hPresentArmEvent, 0);
            ehr = ID3D12Fence_SetEventOnCompletion(q->pDrainFence, v,
                                                   q->hPresentArmEvent);
            t2 = TRITON_PT_NOW();
            /* Packet gate: ask the KMD to park the flip present's own DMA
             * packet on the token the arm just stamped, so the frame-ready
             * signal dxgkrnl/DWM key on stays GPU-true while this thread
             * returns and keeps building the next frame.  NOT_FOUND (no
             * stamp: the arm hit its fast path, or an older KMD) falls back
             * to the CPU wait below for this frame. */
            if (SUCCEEDED(ehr) && q->pDev) {
                VIOGPU_ESCAPE esc;
                memset(&esc, 0, sizeof(esc));
                esc.Type = VIOGPU_PRESENT_GATE_HINT;
                esc.DataLength = 0;
                gated = SUCCEEDED(tritonKmEscape(&q->pDev->Km, &esc));
            }
            const ULONGLONG deadline = GetTickCount64() + 1000;
            while (!gated && SUCCEEDED(ehr) &&
                   ID3D12Fence_GetCompletedValue(q->pDrainFence) < v) {
                if (GetTickCount64() >= deadline) {
                    timedOut = TRUE;
                    break;
                }
                /* Wake on the completion event or the 1 ms slice, whichever
                 * lands first.  The 5 ms cap is a backstop for a swallowed
                 * timer set; a stale timer fire only costs one extra value
                 * check. */
                if (q->hPresentPaceTimer) {
                    LARGE_INTEGER due;
                    due.QuadPart = -10000;   /* 1 ms, relative */
                    HANDLE hs[2] = { q->hPresentArmEvent,
                                     q->hPresentPaceTimer };
                    if (SetWaitableTimer(q->hPresentPaceTimer, &due, 0, NULL,
                                         NULL, FALSE))
                        WaitForMultipleObjects(2, hs, FALSE, 5);
                    else
                        WaitForSingleObject(q->hPresentArmEvent, 1);
                } else {
                    WaitForSingleObject(q->hPresentArmEvent, 1);
                }
            }
        }
        t12PtAccount(t1 - t0, t2 - t1, TRITON_PT_NOW() - t2, already, gated,
                     timedOut);
        if (n <= 8 || (n & 255) == 0 || timedOut)
            TR_LOG("12.PresentArm #%ld: tid=%lu v=%llu signal=0x%08lx "
                   "arm=0x%08lx already=%d gated=%d%s", (long)n,
                   GetCurrentThreadId(), (unsigned long long)v,
                   (unsigned long)shr, (unsigned long)ehr, (int)already,
                   (int)gated,
                   timedOut ? " DEADLINE (presented anyway)" : "");
        if (timedOut)
            npt_log("present arm DEADLINE #%ld v=%llu done=%llu gated=%d",
                    (long)n, (unsigned long long)v,
                    (unsigned long long)
                        ID3D12Fence_GetCompletedValue(q->pDrainFence),
                    (int)gated);
    }

    if (n <= 8 || (n & 255) == 0)
        TR_LOG("12.Present #%ld: src=0x%x dst=0x%x ctx=%p surfaces=%u "
               "flipInterval=%d", (long)n,
               src ? src->hKMAllocation : 0, dst ? dst->hKMAllocation : 0,
               q->hKMContext, pArgs->SurfacesToPresent,
               (int)pArgs->FlipInterval);
}

/* _0028 appends SyncIntervalOverride{Valid,}: left clear, the runtime's
 * own interval stands. */
static VOID APIENTRY
t12Present0028(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HCOMMANDQUEUE hQueue,
               const D3D12DDIARG_PRESENT_0001 *pArgs, D3D12DDI_PRESENT_0028 *pOut)
{
    if (!pOut)
        return;
    pOut->SyncIntervalOverrideValid = FALSE;
    pOut->SyncIntervalOverride = (DXGI_DDI_FLIP_INTERVAL_TYPE)0;
    t12Present(hList, hQueue, pArgs, (D3D12DDI_PRESENT_0003 *)pOut);
}

/* _0051 drops hSrc/hDst/hContext from the out struct: the allocations go
 * to slot 0 of the broadcast arrays and the context to the optional
 * contexts struct (the hardware-queue struct stays empty: no HW queues). */
static VOID APIENTRY
t12Present0051(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HCOMMANDQUEUE hQueue,
               const D3D12DDIARG_PRESENT_0001 *pArgs, D3D12DDI_PRESENT_0051 *pOut,
               D3D12DDI_PRESENT_CONTEXTS_0051 *pContexts,
               D3D12DDI_PRESENT_HWQUEUES_0051 *pHwQueues)
{
    D3D12DDI_PRESENT_0003 legacy;
    if (!pOut)
        return;
    memset(pOut, 0, sizeof(*pOut));
    if (pContexts)
        memset(pContexts, 0, sizeof(*pContexts));
    if (pHwQueues)
        memset(pHwQueues, 0, sizeof(*pHwQueues));
    memset(&legacy, 0, sizeof(legacy));
    t12Present(hList, hQueue, pArgs, &legacy);
    pOut->BroadcastSrcAllocation[0] = legacy.hSrcAllocation;
    pOut->BroadcastDstAllocation[0] = legacy.hDstAllocation;
    pOut->AddedGpuWork          = legacy.AddedGpuWork;
    pOut->BackBufferMultiplicity = legacy.BackBufferMultiplicity;
    if (pContexts) {
        pContexts->hContext = legacy.hContext;
        pContexts->BroadcastContextCount = 0;
    }
}

void
triton12InstallPresentFuncs0062(D3D12DDI_COMMAND_LIST_FUNCS_3D_0062 *t)
{
    t->pfnPresent = t12Present0051;
}

void
triton12InstallPresentFuncs0033(D3D12DDI_COMMAND_LIST_FUNCS_3D_0033 *t)
{
    t->pfnPresent = t12Present0028;
}

void
triton12InstallPresentFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *t)
{
    t->pfnPresent = t12Present;
}
