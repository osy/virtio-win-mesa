/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Centralized transport command dispatchers.  One function per
 * transport command in active use; each owns the cmd struct
 * construction, picks the right submit primitive (raw / raw_seqno /
 * raw_with_payload / submit_command + reply / renderer_submit_cmd),
 * and reads out the reply where applicable.  Callers pass logical
 * inputs and receive logical outputs through the function signature;
 * no callers should construct npt_cmd_* structs directly.
 *
 * Naming mirrors NPT_TRANSPORT_<SUBGROUP>_<METHOD>: e.g.
 * NPT_TRANSPORT_RESOURCE_UPDATE -> npt_dispatch_resource_update.  Where
 * one logical command has two submit-style variants, the seqno-bearing
 * variant gets a _seqno suffix.
 *
 * Scope: cross-file commands only.  Ring-internal commands (RING_*,
 * CORE_SET_REPLY_STREAM, RESOURCE_EXECUTE_CMD_STREAM, COM_RELEASE) stay
 * inline in npt_ring.c because they're emitted under the ring lock by
 * the locked-submit machinery itself.
 */

#ifndef NPT_DISPATCH_H
#define NPT_DISPATCH_H

#include "npt_common.h"

#include "neptune-protocol/npt_protocol_directx_types.h"

struct npt_renderer;
struct npt_ring;

/* ==========================================================================
 * COM
 * ========================================================================== */

HRESULT npt_dispatch_com_query_interface(struct npt_ring *ring,
                                         uint64_t src_id,
                                         const GUID *iid,
                                         uint64_t guest_id);

/* ==========================================================================
 * RESOURCE
 * ========================================================================== */

HRESULT npt_dispatch_resource_map(struct npt_ring *ring,
                                  uint64_t context_id,
                                  uint64_t resource_id,
                                  uint32_t subresource,
                                  uint32_t access_flags,
                                  uint32_t api_map_flags,
                                  uint32_t shmem_res_id,
                                  uint64_t byte_size,
                                  uint32_t mip_height,
                                  uint32_t mip_depth,
                                  uint32_t *out_row_pitch,
                                  uint32_t *out_depth_pitch);

bool npt_dispatch_resource_unmap(struct npt_ring *ring,
                                 uint64_t context_id,
                                 uint64_t resource_id,
                                 uint32_t subresource,
                                 uint32_t shmem_res_id,
                                 uint64_t byte_size,
                                 uint32_t shmem_offset,
                                 uint32_t access_flags);

bool npt_dispatch_resource_unmap_seqno(struct npt_ring *ring,
                                       uint64_t context_id,
                                       uint64_t resource_id,
                                       uint32_t subresource,
                                       uint32_t shmem_res_id,
                                       uint64_t byte_size,
                                       uint32_t shmem_offset,
                                       uint32_t access_flags,
                                       uint32_t *out_seqno);

bool npt_dispatch_resource_update(struct npt_ring *ring,
                                  uint64_t resource_host_id,
                                  uint32_t subresource,
                                  uint32_t row_pitch,
                                  uint32_t depth_pitch,
                                  const D3D11_BOX *box,
                                  const void *data,
                                  uint32_t byte_size);

/* ==========================================================================
 * WSI
 * ========================================================================== */

bool npt_dispatch_wsi_set_preferred_display_info(struct npt_renderer *renderer,
                                                 uint32_t virgl_format,
                                                 uint32_t refresh_num,
                                                 uint32_t refresh_den,
                                                 const char *app_path,
                                                 uint32_t app_path_len);

bool npt_dispatch_wsi_get_swapchain_images(struct npt_ring *ring,
                                           uint64_t swapchain_id,
                                           uint32_t data_res_id,
                                           uint32_t data_off);

bool npt_dispatch_wsi_image_release(struct npt_renderer *renderer,
                                    uint64_t swapchain_id,
                                    uint32_t image_index);

/* ==========================================================================
 * EVENT
 * ========================================================================== */

bool npt_dispatch_event_register(struct npt_renderer *renderer,
                                 uint64_t event_token);

bool npt_dispatch_event_arm_fence(struct npt_ring *ring,
                                  uint64_t event_token,
                                  uint32_t ring_idx);

bool npt_dispatch_event_release(struct npt_renderer *renderer,
                                uint64_t event_token);

/* ==========================================================================
 * FEEDBACK
 * ========================================================================== */

bool npt_dispatch_feedback_register_query(struct npt_ring *ring,
                                          uint64_t query_id,
                                          uint32_t fb_res_id,
                                          uint32_t fb_offset,
                                          uint32_t query_data_size);

bool npt_dispatch_feedback_unregister_query(struct npt_ring *ring,
                                            uint64_t query_id);

bool npt_dispatch_feedback_register_fence(struct npt_ring *ring,
                                          uint64_t fence_id,
                                          uint32_t fb_res_id,
                                          uint32_t fb_offset);

#endif /* NPT_DISPATCH_H */
