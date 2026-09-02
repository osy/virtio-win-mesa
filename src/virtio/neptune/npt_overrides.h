/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Each override family exposes an init() that patches vtbls and/or
 * registers with npt_com_register_family.  All inits run once from
 * npt_com_init_impl after the default-ctor populator.
 */

#ifndef NPT_OVERRIDES_H
#define NPT_OVERRIDES_H

#include <stdbool.h>
#include <stdint.h>

void npt_overrides_d3d11_device_init(void);
void npt_overrides_d3d11_context_init(void);
void npt_overrides_d3d11_buffer_init(void);
void npt_overrides_d3d11_texture_init(void);
void npt_overrides_d3d11_view_init(void);
void npt_overrides_d3d11_fence_init(void);
void npt_overrides_d3d11_query_init(void);
void npt_overrides_dxgi_output_init(void);
void npt_overrides_dxgi_factory_init(void);
void npt_overrides_dxgi_factorymedia_init(void);
void npt_overrides_dxgi_adapter_init(void);
void npt_overrides_d3d12_device_init(void);
void npt_overrides_d3d12_resource_init(void);
void npt_overrides_d3d12_fence_init(void);
void npt_overrides_d3d12_queue_init(void);
void npt_overrides_d3d12_list_init(void);

/* npt_overrides_d3d12_device.c: before a command that reads the CPU
 * descriptor at `handle` at decode time, order `ring` after the ring
 * that last wrote the handle's heap.  See "descriptor heap ordering"
 * there. */
struct npt_ring;
void npt_d3d12_desc_order_read(struct npt_ring *ring, uint64_t handle,
                               uint32_t count);
void npt_d3d12_heap_overrides_init(void);

/* Async-Close barrier stamp for ExecuteCommandLists: true when
 * `list_wrapper` was Closed through the graphics-command-list
 * override, with the recording ring's id + the seqno its head must
 * reach for the Close to be host-decoded. */
bool
npt_d3d12_list_close_barrier(void *list_wrapper, uint64_t *out_ring_id,
                             uint32_t *out_seqno);

/* Execute-barrier stamp for Reset: records {queue ring id, seqno} of
 * the ExecuteCommandLists that referenced `list_wrapper` on the list,
 * on its allocator, and on every bundle it executes (with the bundle's
 * allocator), so a Reset of any of them issued immediately after
 * submission (legal D3D12) waits that ring forward before its own
 * async dispatch.  False when the wrapper has no list aux (QI alias /
 * OOM). */
bool
npt_d3d12_list_stamp_execute(void *list_wrapper, uint64_t ring_id,
                             uint32_t seqno);

struct npt_com_base;
struct npt_output_info;

/* Stamp an output wrapper's aux from connector data; idempotent.
 * Called by IDXGIAdapter::EnumOutputs after npt_com_get_or_wrap. */
void
npt_output_aux_populate(struct npt_com_base *out_com,
                        struct npt_com_base *parent_adapter,
                        uint32_t connector_index,
                        const struct npt_output_info *info);

#endif /* NPT_OVERRIDES_H */
