/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Neptune-internals side of the Triton shared-resource bridge (see
 * tritonSharedBridge.h).  Speaks the Neptune transport (ring, SHARED
 * subgroup dispatch, COM wrapper cache) that the Triton DDI TUs cannot.
 */

#include "tritonSharedBridge.h"

#include <inttypes.h>

#include "npt_com.h"
#include "npt_common.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_renderer.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_defs.h"

bool
tritonSharedBridgeExportBlob(void *pResourceWrapper,
                             struct triton_shared_texture_desc *opts)
{
   if (!pResourceWrapper || !opts)
      return false;

   struct npt_device *dev = npt_com_self_device(pResourceWrapper);
   if (!dev || !dev->renderer)
      return false;
   struct npt_renderer *renderer = dev->renderer;

   const uint64_t texture_id = npt_com_self_id(pResourceWrapper);
   if (!texture_id)
      return false;

   /* Reply window for npt_blob_export_info.  data_off is required: the
    * info shmem comes from a bump-allocated pool, so the host deposits
    * the struct at our offset, not at the resource start. */
   size_t info_off = 0;
   struct npt_renderer_shmem *shm =
      npt_device_alloc_data(dev, sizeof(struct npt_blob_export_info),
                            &info_off);
   if (!shm)
      return false;
   memset((uint8_t *)shm->mmap_ptr + info_off, 0,
          sizeof(struct npt_blob_export_info));

   /* blob_id = the texture's object id: unique within this context,
    * which is the scope of the host's pending-blob table. */
   HRESULT hr = npt_dispatch_shared_export_blob(
      npt_com_self_ring(pResourceWrapper), texture_id, texture_id,
      shm->res_id, (uint32_t)info_off);

   bool ok = false;
   if (NPT_SUCCEEDED(hr)) {
      const struct npt_blob_export_info *info =
         (const struct npt_blob_export_info *)
            ((uint8_t *)shm->mmap_ptr + info_off);
      if (info->plane_count >= 1 &&
          info->plane_count <= NPT_BLOB_EXPORT_MAX_PLANES &&
          info->allocation_size) {
         opts->blob_id = texture_id;
         opts->create_ctx_id = renderer->info.virtio_ctx_id;
         opts->plane_count = info->plane_count;
         opts->texture_layout = info->texture_layout;
         opts->modifier = info->modifier;
         opts->allocation_size = info->allocation_size;
         for (uint32_t i = 0; i < info->plane_count; i++) {
            opts->planes[i].offset = info->planes[i].offset;
            opts->planes[i].pitch = info->planes[i].pitch;
         }
         ok = true;
      } else {
         npt_log("shared bridge: export id 0x%016" PRIx64
                 " returned bad info (planes=%u size=%" PRIu64 ")",
                 texture_id, info->plane_count, info->allocation_size);
      }
   } else {
      npt_log("shared bridge: export id 0x%016" PRIx64 " failed hr=0x%x",
              texture_id, hr);
   }

   npt_renderer_shmem_unref(renderer, shm);
   return ok;
}

bool
tritonSharedBridgeImportRes(void *pDeviceWrapper, uint32_t res_id,
                            uint64_t size, uint32_t *out_alloc,
                            uint32_t *out_res_kmt)
{
   if (!pDeviceWrapper || !res_id || !out_alloc || !out_res_kmt)
      return false;
   struct npt_device *dev = npt_com_self_device(pDeviceWrapper);
   if (!dev || !dev->renderer)
      return false;
   return npt_renderer_import_res(dev->renderer, res_id, size,
                                  out_alloc, out_res_kmt);
}

void
tritonSharedBridgeReleaseImportRes(void *pDeviceWrapper, uint32_t alloc,
                                   uint32_t res_kmt)
{
   if (!pDeviceWrapper)
      return;
   struct npt_device *dev = npt_com_self_device(pDeviceWrapper);
   if (!dev || !dev->renderer)
      return;
   npt_renderer_release_import_res(dev->renderer, alloc, res_kmt);
}

void *
tritonSharedBridgeOpenRes(void *pDeviceWrapper, uint32_t res_id,
                          const struct triton_shared_texture_desc *opts)
{
   if (!pDeviceWrapper || !res_id || !opts)
      return NULL;
   struct npt_device *dev = npt_com_self_device(pDeviceWrapper);
   if (!dev)
      return NULL;
   if (opts->plane_count < 1 ||
       opts->plane_count > NPT_BLOB_EXPORT_MAX_PLANES)
      return NULL;

   /* Mint the consumer-side object id up front; the host registers the
    * imported texture under it, so the wrapper we build below resolves
    * to the same host object on subsequent calls. */
   uint64_t mint = npt_com_allocate_next_id();

   struct npt_cmd_shared_open_res cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.mint_object_id = mint;
   cmd.res_id = res_id;
   cmd.width = opts->width;
   cmd.height = opts->height;
   cmd.mip_levels = opts->mip_levels;
   cmd.array_size = opts->array_size;
   cmd.format = opts->format;
   cmd.sample_count = opts->sample_count;
   cmd.usage = opts->usage;
   cmd.bind_flags = opts->bind_flags;
   cmd.cpu_access_flags = opts->cpu_access_flags;
   cmd.misc_flags = opts->misc_flags;
   cmd.export_info.modifier = opts->modifier;
   cmd.export_info.allocation_size = opts->allocation_size;
   cmd.export_info.plane_count = opts->plane_count;
   cmd.export_info.texture_layout = opts->texture_layout;
   for (uint32_t i = 0; i < opts->plane_count; i++) {
      cmd.export_info.planes[i].offset = opts->planes[i].offset;
      cmd.export_info.planes[i].pitch = opts->planes[i].pitch;
   }

   HRESULT hr = npt_dispatch_shared_open_res(
      npt_com_self_ring(pDeviceWrapper),
      npt_com_self_id(pDeviceWrapper), &cmd);
   if (NPT_FAILED(hr)) {
      npt_log("shared bridge: open res_id=%u failed hr=0x%x", res_id, hr);
      return NULL;
   }

   return npt_com_get_or_wrap(dev, &NPT_IID_ID3D11Texture2D, mint,
                              (struct npt_com_base *)pDeviceWrapper);
}
