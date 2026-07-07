/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGISwapChain overrides.  The host owns no swapchains: presentable
 * surfaces are exportable textures bound to virtio-gpu blob resources,
 * presented by the guest KMD's flip path (native Win32), so there is no
 * host-side dmabuf-WSI flip protocol.
 *
 * Present stays overridden because it is skip_default (its parameters
 * cannot be auto-marshalled) and a wire swapchain, should a caller
 * create one, must not abort the process.  Forward it async and
 * fire-and-forget.
 */

#include "npt_com.h"
#include "npt_overrides.h"
#include "npt_ring.h"

#include "neptune-protocol/npt_protocol_client_idxgiswapchain.h"
#include "neptune-protocol/npt_protocol_guest_idxgiswapchain.h"

static HRESULT NPT_STDMETHODCALLTYPE
sc_Present_async(void *self, UINT SyncInterval, UINT Flags)
{
   npt_async_IDXGISwapChain_Present(npt_com_self_ring(self),
                                    npt_com_self_id(self),
                                    SyncInterval, Flags);
   return S_OK;
}

void
npt_overrides_dxgi_swapchain_init(void)
{
   NPT_REGISTER_OVERRIDE(idxgiswapchain,  Present, sc_Present_async);
   NPT_REGISTER_OVERRIDE(idxgiswapchain1, Present, sc_Present_async);
   NPT_REGISTER_OVERRIDE(idxgiswapchain2, Present, sc_Present_async);
   NPT_REGISTER_OVERRIDE(idxgiswapchain3, Present, sc_Present_async);
   NPT_REGISTER_OVERRIDE(idxgiswapchain4, Present, sc_Present_async);
}
