/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Device-level kernel-context / virtio-context plumbing for the
 * present path.  See tritonPresent.h.
 */

#include "tritonPresent.h"
#include "triton_log.h"

void
tritonPresentEnsureKernelContext(PTRITON_DEVICE pD)
{
   if (pD->hKMContext || !pD->KTCallbacks.pfnCreateContextCb)
      return;
   /* Serialise the check-and-create under kmCtxLock (the HCONTEXT-domain
    * lock, see triton.h): concurrent present threads — and a primary-create
    * on another thread — could each see hKMContext==NULL and create a
    * duplicate kernel context. */
   if (!pD->presentLockInit)
      return;
   EnterCriticalSection(&pD->kmCtxLock);
   if (!pD->hKMContext) {
      /* Prefer a VIRTUAL-ADDRESSING context (WDDM2 GpuMmu).  On a legacy
       * (non-virtual) context, the redirected blt-model present is the
       * first submission whose DMA buffer carries content, and dxgmms2
       * never makes the non-virtual DMA pool resident -- the submit dies
       * with VidSchErrorSubmittingRenderForNonResidentAllocation ("Failed
       * to reference DMA buffer: Allocation is not requested to be
       * resident") and the device reads back DXGI_ERROR_DEVICE_HUNG
       * on the first present of any DISCARD swapchain.  A virtual
       * context's DMA pool is GPU-VA mapped and resident by construction
       * (the D3D12 path submits through it constantly).  Fall back to the
       * legacy callback on failure so WDDM 1.3 keeps its old context. */
      HRESULT hr = E_NOTIMPL;
      if (pD->KTCallbacks.pfnCreateContextVirtualCb) {
         D3DDDICB_CREATECONTEXTVIRTUAL vctx;
         memset(&vctx, 0, sizeof(vctx));
         hr = pD->KTCallbacks.pfnCreateContextVirtualCb(pD->hRTDevice.handle,
                                                        &vctx);
         if (SUCCEEDED(hr)) {
            pD->hKMContext = vctx.hContext;
            TR_LOG("present: kernel context created (virtual)");
         }
      }
      if (!pD->hKMContext) {
         D3DDDICB_CREATECONTEXT ctx;
         memset(&ctx, 0, sizeof(ctx));
         HRESULT lhr = pD->KTCallbacks.pfnCreateContextCb(pD->hRTDevice.handle,
                                                          &ctx);
         if (SUCCEEDED(lhr)) {
            pD->hKMContext = ctx.hContext;
            TR_LOG("present: kernel context created (LEGACY; virtual hr=0x%08lx)",
                   (unsigned long)hr);
         } else
            TR_LOG("present: CreateContext failed (virtual 0x%08lx, "
                   "legacy 0x%08lx)", (unsigned long)hr, (unsigned long)lhr);
      }
   }
   LeaveCriticalSection(&pD->kmCtxLock);
}

HRESULT
tritonPresentEscape(PTRITON_DEVICE pD, VIOGPU_ESCAPE *esc)
{
   if (!pD->KTCallbacks.pfnEscapeCb)
      return E_NOTIMPL;
   D3DDDICB_ESCAPE cb;
   memset(&cb, 0, sizeof(cb));
   cb.hDevice               = pD->hRTDevice.handle;
   cb.pPrivateDriverData    = esc;
   cb.PrivateDriverDataSize = sizeof(*esc);
   return pD->KTCallbacks.pfnEscapeCb(pD->pAdapter->hRTAdapter.handle, &cb);
}

/* WDDM2 explicit residency.  GpuMmu allocations are NOT resident at
 * create, and dxgmms2 refuses to reference an allocation from a DMA
 * buffer unless residency was REQUESTED beforehand -- its demand fault
 * only pages in allocations that already hold a request (it happily
 * MakeResident-ed the win32k staging surface of a redirected present
 * while rejecting our back buffer in the same packet).  The OS itself
 * builds such a reference for every redirected blt-model present
 * (source = the back buffer), which then dies with
 * VidSchErrorSubmittingRenderForNonResidentAllocation ("Failed to
 * reference DMA buffer: Allocation is not requested to be resident")
 * and the app reads back DEVICE_HUNG on the first frame of any
 * DISCARD swapchain.  Rendering rides the npt
 * ring and references no allocations, so these requests exist for the
 * OS's benefit, not our own submissions; the paging fence the request
 * returns can be ignored -- dxgmms orders its own present packet
 * against outstanding paging operations.
 *
 * The paging queue is created lazily on the first request.  On WDDM
 * 1.3 the runtime leaves pfnCreatePagingQueueCb NULL (or the create
 * fails on a non-GpuMmu adapter); PagingQueueTried latches either
 * outcome and every request becomes a no-op -- patch-location lists
 * keep their implicit residency there. */
void
tritonPresentRequestResidency(PTRITON_DEVICE pD, D3DKMT_HANDLE hAllocation)
{
   if (!hAllocation)
      return;
   if (!pD->PagingQueueTried) {
      if (!pD->presentLockInit)
         return;
      EnterCriticalSection(&pD->kmCtxLock);
      if (!pD->PagingQueueTried) {
         if (pD->KTCallbacks.pfnCreatePagingQueueCb &&
             pD->KTCallbacks.pfnMakeResidentCb) {
            D3DDDICB_CREATEPAGINGQUEUE pq;
            memset(&pq, 0, sizeof(pq)); /* PRIORITY_NORMAL, adapter 0 */
            HRESULT hr = pD->KTCallbacks.pfnCreatePagingQueueCb(
               pD->hRTDevice.handle, &pq);
            if (SUCCEEDED(hr) && pq.hPagingQueue) {
               pD->hPagingQueue  = pq.hPagingQueue;
               pD->PagingFenceVa = (volatile const UINT64 *)
                  pq.FenceValueCPUVirtualAddress;
               TR_LOG("present: paging queue created (fence va=%p)",
                      pq.FenceValueCPUVirtualAddress);
            } else
               TR_LOG("present: CreatePagingQueue failed 0x%08lx -- "
                      "legacy (implicit) residency", (unsigned long)hr);
         }
         pD->PagingQueueTried = TRUE;
      }
      LeaveCriticalSection(&pD->kmCtxLock);
   }
   if (!pD->hPagingQueue)
      return;

   D3DDDI_MAKERESIDENT mr;
   memset(&mr, 0, sizeof(mr));
   mr.hPagingQueue   = pD->hPagingQueue;
   mr.NumAllocations = 1;
   mr.AllocationList = &hAllocation;
   HRESULT hr = pD->KTCallbacks.pfnMakeResidentCb(pD->hRTDevice.handle, &mr);
   if (hr == E_PENDING && pD->PagingFenceVa) {
      /* Queued: the residency operation completes when the paging
       * queue's monitored fence reaches mr.PagingFenceValue.  A present
       * built against the allocation BEFORE that point still dies with
       * "not requested to be resident", which leaves a composed
       * swapchain permanently black, so wait it out.  The
       * fence value VA is a plain monotonic UINT64; creation-time waits
       * are rare and short, so poll with a yield and a loud timeout. */
      ULONGLONG deadline = GetTickCount64() + 5000;
      while (*pD->PagingFenceVa < mr.PagingFenceValue) {
         if (GetTickCount64() > deadline) {
            TR_LOG("present: paging fence wait TIMED OUT (alloc=0x%x "
                   "fence=%llu < %llu)", hAllocation,
                   (unsigned long long)*pD->PagingFenceVa,
                   (unsigned long long)mr.PagingFenceValue);
            return;
         }
         Sleep(0);
      }
      TR_LOG("present: MakeResident(alloc=0x%x) completed async "
             "(fence %llu)", hAllocation,
             (unsigned long long)mr.PagingFenceValue);
   } else if (FAILED(hr))
      TR_LOG("present: MakeResident(alloc=0x%x) FAILED 0x%08lx -- an OS "
             "present referencing it will hang the device",
             hAllocation, (unsigned long)hr);
}

void
tritonPresentEnsureRuntimeCtx(PTRITON_DEVICE pD)
{
   if (pD->RuntimeCtxInited)
      return;
   /* Serialise the check-and-set under kmCtxLock: racing first-uses would
    * double-issue VIOGPU_CTX_INIT.  pfnEscapeCb is in the HCONTEXT-callback
    * family (see triton.h). */
   if (!pD->presentLockInit)
      return;
   EnterCriticalSection(&pD->kmCtxLock);
   if (!pD->RuntimeCtxInited) {
      VIOGPU_ESCAPE esc;
      memset(&esc, 0, sizeof(esc));
      esc.Type             = VIOGPU_CTX_INIT;
      esc.DataLength       = sizeof(esc.CtxInit);
      esc.CtxInit.CapsetID = 7;
      esc.CtxInit.NumRings = 1;
      /* Latch only on success: every later blob create and context attach
       * depends on this context existing, so a failure must stay retryable
       * rather than silently disabling them for the device's lifetime. */
      HRESULT hr = tritonPresentEscape(pD, &esc);
      if (SUCCEEDED(hr))
         pD->RuntimeCtxInited = TRUE;
      else
         TR_LOG("present: VIOGPU_CTX_INIT failed 0x%08lx", (unsigned long)hr);
   }
   LeaveCriticalSection(&pD->kmCtxLock);
}
