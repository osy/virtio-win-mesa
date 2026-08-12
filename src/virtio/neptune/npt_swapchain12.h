/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-fabricated IDXGISwapChain construction for the D3D12 (flip-model)
 * presentation path on Wine.
 */

#ifndef NPT_SWAPCHAIN12_H
#define NPT_SWAPCHAIN12_H

#include <stdint.h>

#include "neptune-protocol/npt_protocol_directx_types.h"

/*
 * Build a guest-fabricated IDXGISwapChain (all tiers) over queue_unknown
 * (any Neptune wrapper QI-able to ID3D12CommandQueue).  The swapchain
 * holds the queue, the ID3D12Device resolved through the queue's
 * GetDevice, and factory_wrapper (may be NULL, retained for GetParent).
 * On Wine this is the real presentation path (X11 DRI3/Present);
 * elsewhere it returns E_NOTIMPL, matching the host's refusal.
 * *out_swapchain is the wrapper (usable as any IDXGISwapChain* tier)
 * with one public reference.
 */
HRESULT
npt_guest_swapchain12_create(void *queue_unknown,
                             const DXGI_SWAP_CHAIN_DESC1 *desc1,
                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
                             HWND hwnd,
                             void *factory_wrapper,
                             void **out_swapchain);

/* DXGI_SWAP_CHAIN_DESC (v0) convenience wrapper for CreateSwapChain
 * called with an ID3D12CommandQueue. */
HRESULT
npt_guest_swapchain12_create_legacy(void *queue_unknown,
                                    const DXGI_SWAP_CHAIN_DESC *desc,
                                    void *factory_wrapper,
                                    void **out_swapchain);

#endif /* NPT_SWAPCHAIN12_H */
