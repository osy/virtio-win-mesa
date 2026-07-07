/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_dispatch.h"

#include "npt_renderer.h"
#include "npt_ring.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_defs.h"

#include <string.h>

/* ==========================================================================
 * COM
 * ========================================================================== */

HRESULT
npt_dispatch_com_query_interface(struct npt_ring *ring, uint64_t src_id,
                                 const GUID *iid, uint64_t guest_id)
{
   if (!ring || !src_id || !iid || !guest_id)
      return NPT_E_FAIL;

   struct npt_cmd_com_query_interface cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_COM,
                             NPT_TRANSPORT_COM_QUERY_INTERFACE);
   cmd.header.cmd_flags = NPT_CMD_FLAG_REPLY;
   cmd.header.cmd_size = sizeof(cmd);
   cmd.header.object_id = src_id;
   cmd.iid = *iid;
   cmd.guest_id = guest_id;

   struct npt_ring_submit_command submit;
   memset(&submit, 0, sizeof(submit));
   struct npt_cs_encoder *enc = npt_ring_submit_command_init(
      ring, &submit, &cmd, sizeof(cmd),
      sizeof(struct npt_cmd_com_query_interface_reply));
   if (enc)
      enc->cur = (uint8_t *)&cmd + sizeof(cmd);
   npt_ring_submit_command(ring, &submit);

   struct npt_cs_decoder *dec = npt_ring_get_command_reply(ring, &submit);
   HRESULT hr = NPT_E_FAIL;
   if (dec && dec->cur && dec->end &&
       (size_t)(dec->end - dec->cur) >=
          sizeof(struct npt_cmd_com_query_interface_reply)) {
      const struct npt_cmd_com_query_interface_reply *reply =
         (const struct npt_cmd_com_query_interface_reply *)dec->cur;
      hr = (HRESULT)reply->header.cmd_return;
   }
   if (dec)
      npt_ring_free_command_reply(ring, &submit);

   return hr;
}

/* ==========================================================================
 * RESOURCE
 * ========================================================================== */

HRESULT
npt_dispatch_resource_map(struct npt_ring *ring, uint64_t context_id,
                          uint64_t resource_id, uint32_t subresource,
                          uint32_t access_flags, uint32_t api_map_flags,
                          uint32_t shmem_res_id, uint64_t byte_size,
                          uint32_t mip_height, uint32_t mip_depth,
                          uint32_t shmem_offset,
                          uint32_t *out_row_pitch, uint32_t *out_depth_pitch)
{
   struct npt_cmd_map_resource cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RESOURCE,
                             NPT_TRANSPORT_RESOURCE_MAP);
   cmd.header.cmd_flags = NPT_CMD_FLAG_REPLY;
   cmd.header.cmd_size = sizeof(cmd);
   cmd.context_id = context_id;
   cmd.resource_id = resource_id;
   cmd.subresource = subresource;
   cmd.access_flags = access_flags;
   cmd.api_map_flags = api_map_flags;
   cmd.shmem_res_id = shmem_res_id;
   cmd.byte_size = byte_size;
   cmd.mip_height = mip_height;
   cmd.mip_depth = mip_depth;
   cmd.shmem_offset = shmem_offset;

   struct npt_ring_submit_command submit;
   memset(&submit, 0, sizeof(submit));
   struct npt_cs_encoder *enc = npt_ring_submit_command_init(
      ring, &submit, &cmd, sizeof(cmd),
      sizeof(struct npt_cmd_map_resource_reply));
   if (enc)
      enc->cur = (uint8_t *)&cmd + sizeof(cmd);
   npt_ring_submit_command(ring, &submit);

   struct npt_cs_decoder *dec = npt_ring_get_command_reply(ring, &submit);
   HRESULT hr = NPT_E_FAIL;
   if (dec && dec->cur && dec->end &&
       (size_t)(dec->end - dec->cur) >=
          sizeof(struct npt_cmd_map_resource_reply)) {
      const struct npt_cmd_map_resource_reply *reply =
         (const struct npt_cmd_map_resource_reply *)dec->cur;
      hr = (HRESULT)reply->header.cmd_return;
      if (NPT_SUCCEEDED(hr)) {
         if (out_row_pitch)   *out_row_pitch   = reply->row_pitch;
         if (out_depth_pitch) *out_depth_pitch = reply->depth_pitch;
      }
   }
   if (dec)
      npt_ring_free_command_reply(ring, &submit);

   return hr;
}

static void
fill_unmap_cmd(struct npt_cmd_unmap_resource *cmd, uint64_t context_id,
               uint64_t resource_id, uint32_t subresource,
               uint32_t shmem_res_id, uint64_t byte_size,
               uint32_t shmem_offset, uint32_t access_flags)
{
   memset(cmd, 0, sizeof(*cmd));
   cmd->header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RESOURCE,
                             NPT_TRANSPORT_RESOURCE_UNMAP);
   cmd->header.cmd_size = sizeof(*cmd);
   cmd->context_id = context_id;
   cmd->resource_id = resource_id;
   cmd->subresource = subresource;
   cmd->shmem_res_id = shmem_res_id;
   cmd->byte_size = byte_size;
   cmd->shmem_offset = shmem_offset;
   cmd->access_flags = access_flags;
}

bool
npt_dispatch_resource_unmap(struct npt_ring *ring, uint64_t context_id,
                            uint64_t resource_id, uint32_t subresource,
                            uint32_t shmem_res_id, uint64_t byte_size,
                            uint32_t shmem_offset, uint32_t access_flags)
{
   struct npt_cmd_unmap_resource cmd;
   fill_unmap_cmd(&cmd, context_id, resource_id, subresource, shmem_res_id,
                  byte_size, shmem_offset, access_flags);
   return npt_ring_submit_raw(ring, &cmd, sizeof(cmd));
}

bool
npt_dispatch_resource_unmap_seqno(struct npt_ring *ring, uint64_t context_id,
                                  uint64_t resource_id, uint32_t subresource,
                                  uint32_t shmem_res_id, uint64_t byte_size,
                                  uint32_t shmem_offset, uint32_t access_flags,
                                  uint32_t *out_seqno)
{
   struct npt_cmd_unmap_resource cmd;
   fill_unmap_cmd(&cmd, context_id, resource_id, subresource, shmem_res_id,
                  byte_size, shmem_offset, access_flags);
   return npt_ring_submit_raw_seqno(ring, &cmd, sizeof(cmd), out_seqno);
}

bool
npt_dispatch_resource_update(struct npt_ring *ring, uint64_t resource_host_id,
                             uint32_t subresource, uint32_t row_pitch,
                             uint32_t depth_pitch, const D3D11_BOX *box,
                             const void *data, uint32_t byte_size)
{
   if (!ring || !resource_host_id || !data || !byte_size)
      return false;

   const uint32_t payload_aligned = (byte_size + 7u) & ~7u;
   const uint32_t cmd_size = (uint32_t)sizeof(struct npt_cmd_resource_update) +
                             payload_aligned;

   struct npt_cmd_resource_update cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RESOURCE,
                             NPT_TRANSPORT_RESOURCE_UPDATE);
   cmd.header.object_id = resource_host_id;
   cmd.header.cmd_size = cmd_size;
   cmd.subresource = subresource;
   cmd.row_pitch = row_pitch;
   cmd.depth_pitch = depth_pitch;
   cmd.byte_size = byte_size;
   if (box) {
      cmd.has_box    = 1;
      cmd.box_left   = box->left;
      cmd.box_top    = box->top;
      cmd.box_front  = box->front;
      cmd.box_right  = box->right;
      cmd.box_bottom = box->bottom;
      cmd.box_back   = box->back;
   }

   /* Padding bytes between byte_size and payload_aligned are
    * undefined; the host uses cmd.byte_size. */
   return npt_ring_submit_raw_with_payload(ring, &cmd, sizeof(cmd),
                                           data, byte_size, payload_aligned);
}

/* ==========================================================================
 * SHARED
 * ========================================================================== */

HRESULT
npt_dispatch_shared_export_blob(struct npt_ring *ring, uint64_t texture_id,
                                uint64_t blob_id, uint32_t data_res_id,
                                uint32_t data_off)
{
   struct npt_cmd_shared_export_blob cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_SHARED,
                             NPT_TRANSPORT_SHARED_EXPORT_BLOB);
   cmd.header.cmd_flags = NPT_CMD_FLAG_REPLY;
   cmd.header.cmd_size = sizeof(cmd);
   cmd.header.object_id = texture_id;
   cmd.blob_id = blob_id;
   cmd.data_res_id = data_res_id;
   cmd.data_off = data_off;

   struct npt_ring_submit_command submit;
   memset(&submit, 0, sizeof(submit));
   struct npt_cs_encoder *enc = npt_ring_submit_command_init(
      ring, &submit, &cmd, sizeof(cmd),
      sizeof(struct npt_cmd_shared_export_blob_reply));
   if (enc)
      enc->cur = (uint8_t *)&cmd + sizeof(cmd);
   npt_ring_submit_command(ring, &submit);

   struct npt_cs_decoder *dec = npt_ring_get_command_reply(ring, &submit);
   HRESULT hr = NPT_E_FAIL;
   if (dec && dec->cur && dec->end &&
       (size_t)(dec->end - dec->cur) >=
          sizeof(struct npt_cmd_shared_export_blob_reply)) {
      const struct npt_cmd_shared_export_blob_reply *reply =
         (const struct npt_cmd_shared_export_blob_reply *)dec->cur;
      hr = (HRESULT)reply->header.cmd_return;
   }
   if (dec)
      npt_ring_free_command_reply(ring, &submit);

   return hr;
}

HRESULT
npt_dispatch_shared_open_res(struct npt_ring *ring, uint64_t device_id,
                             struct npt_cmd_shared_open_res *cmd)
{
   memset(&cmd->header, 0, sizeof(cmd->header));
   cmd->header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_SHARED,
                             NPT_TRANSPORT_SHARED_OPEN_RES);
   cmd->header.cmd_flags = NPT_CMD_FLAG_REPLY;
   cmd->header.cmd_size = sizeof(*cmd);
   cmd->header.object_id = device_id;

   struct npt_ring_submit_command submit;
   memset(&submit, 0, sizeof(submit));
   struct npt_cs_encoder *enc = npt_ring_submit_command_init(
      ring, &submit, cmd, sizeof(*cmd),
      sizeof(struct npt_cmd_shared_open_res_reply));
   if (enc)
      enc->cur = (uint8_t *)cmd + sizeof(*cmd);
   npt_ring_submit_command(ring, &submit);

   struct npt_cs_decoder *dec = npt_ring_get_command_reply(ring, &submit);
   HRESULT hr = NPT_E_FAIL;
   if (dec && dec->cur && dec->end &&
       (size_t)(dec->end - dec->cur) >=
          sizeof(struct npt_cmd_shared_open_res_reply)) {
      const struct npt_cmd_shared_open_res_reply *reply =
         (const struct npt_cmd_shared_open_res_reply *)dec->cur;
      hr = (HRESULT)reply->header.cmd_return;
   }
   if (dec)
      npt_ring_free_command_reply(ring, &submit);

   return hr;
}

/* ==========================================================================
 * EVENT
 * ========================================================================== */

bool
npt_dispatch_event_register(struct npt_renderer *renderer, uint64_t event_token)
{
   struct npt_cmd_register_event cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_EVENT,
                             NPT_TRANSPORT_EVENT_REGISTER);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.event_token = event_token;
   return npt_renderer_submit_cmd(renderer, &cmd, sizeof(cmd));
}

/* Same command as npt_dispatch_event_register, but submitted fire-and-forget on
 * the method ring instead of the renderer command stream; the synchronous
 * ARM_EVENT_FENCE that follows on the same ring flushes it.  See the header for
 * the ordering this buys. */
bool
npt_dispatch_event_register_ring(struct npt_ring *ring, uint64_t event_token)
{
   struct npt_cmd_register_event cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_EVENT,
                             NPT_TRANSPORT_EVENT_REGISTER);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.event_token = event_token;
   return npt_ring_submit_raw(ring, &cmd, sizeof(cmd));
}

bool
npt_dispatch_event_arm_fence(struct npt_ring *ring, uint64_t event_token,
                             uint32_t ring_idx)
{
   struct npt_cmd_arm_event_fence cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_EVENT,
                             NPT_TRANSPORT_EVENT_ARM_FENCE);
   cmd.header.cmd_flags = NPT_CMD_FLAG_REPLY;
   cmd.header.cmd_size = sizeof(cmd);
   cmd.event_token = event_token;
   cmd.ring_idx = ring_idx;

   struct npt_ring_submit_command submit;
   memset(&submit, 0, sizeof(submit));
   struct npt_cs_encoder *enc = npt_ring_submit_command_init(
      ring, &submit, &cmd, sizeof(cmd),
      sizeof(struct npt_cmd_arm_event_fence_reply));
   if (enc)
      enc->cur = (uint8_t *)&cmd + sizeof(cmd);
   npt_ring_submit_command(ring, &submit);

   struct npt_cs_decoder *dec = npt_ring_get_command_reply(ring, &submit);
   bool ok = false;
   if (dec && dec->cur && dec->end &&
       (size_t)(dec->end - dec->cur) >=
          sizeof(struct npt_cmd_arm_event_fence_reply)) {
      const struct npt_cmd_arm_event_fence_reply *r =
         (const struct npt_cmd_arm_event_fence_reply *)dec->cur;
      ok = (r->header.cmd_return == 0);
   }
   if (dec)
      npt_ring_free_command_reply(ring, &submit);
   return ok;
}

bool
npt_dispatch_event_release(struct npt_renderer *renderer, uint64_t event_token)
{
   struct npt_cmd_release_event cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_EVENT,
                             NPT_TRANSPORT_EVENT_RELEASE);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.event_token = event_token;
   return npt_renderer_submit_cmd(renderer, &cmd, sizeof(cmd));
}

/* Ring-borne RELEASE_EVENT -- counterpart to npt_dispatch_event_register_ring.
 * See the header for why RELEASE must share the method ring with REGISTER. */
bool
npt_dispatch_event_release_ring(struct npt_ring *ring, uint64_t event_token)
{
   struct npt_cmd_release_event cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_EVENT,
                             NPT_TRANSPORT_EVENT_RELEASE);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.event_token = event_token;
   return npt_ring_submit_raw(ring, &cmd, sizeof(cmd));
}

/* ==========================================================================
 * FEEDBACK
 * ========================================================================== */

bool
npt_dispatch_feedback_register_query(struct npt_ring *ring, uint64_t query_id,
                                     uint32_t fb_res_id, uint32_t fb_offset,
                                     uint32_t query_data_size)
{
   struct npt_cmd_register_query_feedback cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_FEEDBACK,
                             NPT_TRANSPORT_FEEDBACK_REGISTER_QUERY);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.header.object_id = query_id;
   cmd.fb_res_id = fb_res_id;
   cmd.fb_offset = fb_offset;
   cmd.query_data_size = query_data_size;
   return npt_ring_submit_raw(ring, &cmd, sizeof(cmd));
}

bool
npt_dispatch_feedback_unregister_query(struct npt_ring *ring, uint64_t query_id)
{
   struct npt_cmd_unregister_query_feedback cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_FEEDBACK,
                             NPT_TRANSPORT_FEEDBACK_UNREGISTER_QUERY);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.header.object_id = query_id;
   return npt_ring_submit_raw(ring, &cmd, sizeof(cmd));
}

bool
npt_dispatch_feedback_register_fence(struct npt_ring *ring, uint64_t fence_id,
                                     uint32_t fb_res_id, uint32_t fb_offset)
{
   struct npt_cmd_register_fence_feedback cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_FEEDBACK,
                             NPT_TRANSPORT_FEEDBACK_REGISTER_FENCE);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.header.object_id = fence_id;
   cmd.fb_res_id = fb_res_id;
   cmd.fb_offset = fb_offset;
   return npt_ring_submit_raw(ring, &cmd, sizeof(cmd));
}
