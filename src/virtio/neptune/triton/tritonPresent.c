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
   D3DDDICB_CREATECONTEXT ctx;
   memset(&ctx, 0, sizeof(ctx));
   HRESULT hr = pD->KTCallbacks.pfnCreateContextCb(pD->hRTDevice.handle, &ctx);
   if (SUCCEEDED(hr))
      pD->hKMContext = ctx.hContext;
   else
      TR_LOG("present: pfnCreateContextCb failed 0x%08lx", hr);
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

void
tritonPresentEnsureRuntimeCtx(PTRITON_DEVICE pD)
{
   if (pD->RuntimeCtxInited)
      return;
   pD->RuntimeCtxInited = TRUE;
   VIOGPU_ESCAPE esc;
   memset(&esc, 0, sizeof(esc));
   esc.Type             = VIOGPU_CTX_INIT;
   esc.DataLength       = sizeof(esc.CtxInit);
   esc.CtxInit.CapsetID = 7;
   esc.CtxInit.NumRings = 1;
   (void)tritonPresentEscape(pD, &esc);
}
