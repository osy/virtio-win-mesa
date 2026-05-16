/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGIFactoryMedia overrides that strip pRestrictToOutput so guest-fab
 * IDXGIOutput wrappers (see npt_overrides_dxgi_output.c) never reach the
 * host.  Decode swapchains have no restrict-to-output getter; the input
 * arg is silently dropped on that path.
 */

#include "npt_com.h"
#include "npt_overrides.h"
#include "npt_device.h"

#include "neptune-protocol/npt_protocol_client_idxgifactorymedia.h"
#include "neptune-protocol/npt_protocol_defs.h"

static HRESULT NPT_STDMETHODCALLTYPE
facm_CreateSwapChainForCompositionSurfaceHandle_override(
   void *self, IUnknown *pDevice, HANDLE hSurface,
   const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput,
   IDXGISwapChain1 **ppSwapChain)
{
   (void)pRestrictToOutput;
   return npt_idxgifactorymedia_default_CreateSwapChainForCompositionSurfaceHandle(
      self, pDevice, hSurface, pDesc, NULL, ppSwapChain);
}

static HRESULT NPT_STDMETHODCALLTYPE
facm_CreateDecodeSwapChainForCompositionSurfaceHandle_override(
   void *self, IUnknown *pDevice, HANDLE hSurface,
   DXGI_DECODE_SWAP_CHAIN_DESC *pDesc, IDXGIResource *pYuvDecodeBuffers,
   IDXGIOutput *pRestrictToOutput, IDXGIDecodeSwapChain **ppSwapChain)
{
   (void)pRestrictToOutput;
   return npt_idxgifactorymedia_default_CreateDecodeSwapChainForCompositionSurfaceHandle(
      self, pDevice, hSurface, pDesc, pYuvDecodeBuffers, NULL, ppSwapChain);
}

static const GUID *const factorymedia_tiers[] = {
   &NPT_IID_IDXGIFactoryMedia, NULL,
};

void
npt_overrides_dxgi_factorymedia_init(void)
{
   NPT_REGISTER_OVERRIDE(idxgifactorymedia,
      CreateSwapChainForCompositionSurfaceHandle,
      facm_CreateSwapChainForCompositionSurfaceHandle_override);
   NPT_REGISTER_OVERRIDE(idxgifactorymedia,
      CreateDecodeSwapChainForCompositionSurfaceHandle,
      facm_CreateDecodeSwapChainForCompositionSurfaceHandle_override);

   npt_com_register_family(factorymedia_tiers, 0, NULL);
}
