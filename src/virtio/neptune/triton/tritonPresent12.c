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
 * (tritonRegisterSharedBlob); the host exports its resources through
 * ID3D12Device::CreateSharedHandle natively.
 */

#include "triton12.h"
#include "tritonSharedBridge.h"
#include "npt_shared_texture.h"
#include "virtio/virtio-gpu/wddm_hw.h"   /* VIOGPU escape / allocation ABI */
#include "triton_log.h"

#include "npt_env.h"
#include "tritonPresentTiming.h"

static HRESULT
t12Escape(PTRITON12_DEVICE p, VIOGPU_ESCAPE *esc)
{
    if (!p->KTCallbacks.pfnEscapeCb || !p->pAdapter)
        return E_NOTIMPL;
    D3DDDICB_ESCAPE cb;
    memset(&cb, 0, sizeof(cb));
    cb.hDevice               = p->hRTDevice.handle;
    cb.pPrivateDriverData    = esc;
    cb.PrivateDriverDataSize = sizeof(*esc);
    return p->KTCallbacks.pfnEscapeCb(p->pAdapter->hRTAdapter.handle, &cb);
}

static void
t12EnsureRuntimeCtx(PTRITON12_DEVICE p)
{
    if (p->RuntimeCtxInited)
        return;
    p->RuntimeCtxInited = TRUE;
    VIOGPU_ESCAPE esc;
    memset(&esc, 0, sizeof(esc));
    esc.Type             = VIOGPU_CTX_INIT;
    esc.DataLength       = sizeof(esc.CtxInit);
    esc.CtxInit.CapsetID = 7;
    esc.CtxInit.NumRings = 1;
    (void)t12Escape(p, &esc);
}

BOOL
triton12RegisterSharedBlob(PTRITON12_DEVICE p, PTRITON12_RESOURCE r,
                           BOOL primary)
{
    /* D3D12 leaves the legacy D3DDDI_DEVICECALLBACKS.pfnAllocateCb NULL;
     * allocation rides the corelayer callback instead. */
    if (!p->pUMCallbacks || !p->pUMCallbacks->pfnAllocateCb || !r->pResource)
        return FALSE;

    t12EnsureRuntimeCtx(p);

    VIOGPU_CREATE_ALLOCATION_EXCHANGE ax;
    memset(&ax, 0, sizeof(ax));
    ax.Type = VIOGPU_RESOURCE_TYPE_SHARED;
    VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *o = &ax.OptionsShared;

    struct triton_shared_texture_desc exp;
    memset(&exp, 0, sizeof(exp));
    if (!tritonSharedBridgeExportBlob(r->pResource, &exp)) {
        TR_LOG("12.shared: export failed %llux%u fmt=%d",
               (unsigned long long)r->Desc.Width, r->Desc.Height,
               (int)r->Desc.Format);
        return FALSE;
    }
    o->blob_id         = exp.blob_id;
    o->create_ctx_id   = exp.create_ctx_id;
    o->plane_count     = exp.plane_count;
    o->texture_layout  = exp.texture_layout;
    o->modifier        = exp.modifier;
    o->allocation_size = exp.allocation_size;
    for (UINT i = 0; i < exp.plane_count && i < 4; i++) {
        o->planes[i].offset = exp.planes[i].offset;
        o->planes[i].pitch  = exp.planes[i].pitch;
    }

    o->primary          = primary ? 1u : 0u;
    o->width            = (ULONG)r->Desc.Width;
    o->height           = r->Desc.Height;
    o->mip_levels       = r->Desc.MipLevels ? r->Desc.MipLevels : 1;
    o->array_size       = r->Desc.DepthOrArraySize ? r->Desc.DepthOrArraySize : 1;
    o->format           = (ULONG)r->Desc.Format;
    o->sample_count     = r->Desc.SampleDesc.Count ? r->Desc.SampleDesc.Count : 1;
    o->usage            = 0;    /* D3D11_USAGE_DEFAULT */
    o->bind_flags       = 0x28; /* RENDER_TARGET | SHADER_RESOURCE */
    o->cpu_access_flags = 0;
    o->misc_flags       = 0x2;  /* D3D11_RESOURCE_MISC_SHARED */

    if (primary) {
        o->ScanoutInfo.width      = (ULONG)r->Desc.Width;
        o->ScanoutInfo.height     = r->Desc.Height;
        o->ScanoutInfo.format     = npt_shared_texture_virgl_format(r->Desc.Format);
        o->ScanoutInfo.strides[0] = (ULONG)o->planes[0].pitch;
        o->ScanoutInfo.offsets[0] = (ULONG)o->planes[0].offset;
    }

    ax.Size = (o->allocation_size + 4095ull) & ~4095ull;
    if (!ax.Size)
        ax.Size = (((ULONGLONG)r->Desc.Width * r->Desc.Height * 4) + 4095)
                  & ~4095ull;

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
    r->hKMAllocation = ai.hAllocation;
    TR_LOG("12.shared: exporter blob_id=0x%llx alloc=0x%x %llux%u primary=%d",
           (unsigned long long)o->blob_id, ai.hAllocation,
           (unsigned long long)r->Desc.Width, r->Desc.Height, primary);
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
    if (!r->hKMAllocation)
        triton12RegisterSharedBlob(p, r, FALSE);
    TR_LOG("12.CheckResourceAllocationHandle: r=%p -> 0x%x", (void *)r,
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

static VOID APIENTRY
t12Present(D3D12DDI_HCOMMANDLIST hList, D3D12DDI_HCOMMANDQUEUE hQueue,
           const D3D12DDIARG_PRESENT_0001 *pArgs, D3D12DDI_PRESENT_0003 *pOut)
{
    PTRITON12_QUEUE q = (PTRITON12_QUEUE)hQueue.pDrvPrivate;
    (void)hList;
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

    pOut->hSrcAllocation = src ? src->hKMAllocation : 0;
    pOut->hDstAllocation = dst ? dst->hKMAllocation : 0;
    pOut->hContext       = q->hKMContext;
    pOut->AddedGpuWork   = FALSE;
    pOut->BackBufferMultiplicity = 1;

    static LONG s_presentN;
    LONG n = InterlockedIncrement(&s_presentN);

    /* Arm the render->flip token for this frame, then wait for it.
     *
     * ARM.  Without this the KMD's TakeThreadToken() finds no stamp for the
     * flip that follows, gates it on token 0 -- which always reads retired --
     * and promotes the scanout immediately.  The D3D11 path has always armed
     * here (tritonPresentFlushAndGate in tritonDxgi.c, "so the KMD never
     * scans out a half-written backbuffer"); D3D12 never did, so every D3D12
     * title presented un-gated.
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
    if (q->pQueue && q->pDrainFence && q->hPresentArmEvent) {
        const UINT64 v =
            (UINT64)InterlockedIncrement64((volatile LONG64 *)&q->DrainValue);
        const double t0 = TRITON_PT_NOW();
        HRESULT shr = ID3D12CommandQueue_Signal(q->pQueue, q->pDrainFence, v);
        if (SUCCEEDED(shr))
            t12PublishDrainVisible(q, v);
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
                gated = SUCCEEDED(t12Escape(q->pDev, &esc));
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
    } else if (n <= 8) {
        TR_LOG("12.PresentArm #%ld: SKIPPED (queue=%p fence=%p evt=%p)",
               (long)n, (void *)q->pQueue, (void *)q->pDrainFence,
               (void *)q->hPresentArmEvent);
    }

    if (n <= 8 || (n & 255) == 0)
        TR_LOG("12.Present #%ld: src=0x%x dst=0x%x ctx=%p surfaces=%u "
               "flipInterval=%d", (long)n,
               src ? src->hKMAllocation : 0, dst ? dst->hKMAllocation : 0,
               q->hKMContext, pArgs->SurfacesToPresent,
               (int)pArgs->FlipInterval);
}

void
triton12InstallPresentFuncs(D3D12DDI_COMMAND_LIST_FUNCS_3D_0022 *t)
{
    t->pfnPresent = t12Present;
}
