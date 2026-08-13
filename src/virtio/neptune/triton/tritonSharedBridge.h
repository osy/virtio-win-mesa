/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Bridge between Triton's shared/presentable-texture path and the
 * Neptune transport internals.  Triton owns the WDDM side (the shared
 * kernel allocation whose private data conveys the texture description
 * to the opening process) and the COM side it reaches with COBJMACROS;
 * the SHARED transport commands and host-object wrapping that need
 * Neptune-layer internals live here behind a stdint-only interface, so
 * the Triton TUs need not include the npt headers and this TU need not
 * include the WDDM ones.
 */

#ifndef TRITON_SHARED_BRIDGE_H
#define TRITON_SHARED_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRITON_SHARED_MAX_PLANES 4

/* Host capset capability bits carried by triton_adapter_probe.caps_flags
 * (mirror of npt_common.h NPT_CAPSET_CAP_* -- Triton TUs cannot include
 * npt headers; tritonSharedBridge.c static_asserts the two copies
 * together).  A clear bit always means "not supported". */
#define TRITON_HOSTCAP_D3D12                     (1u << 0)
#define TRITON_HOSTCAP_TBDR                      (1u << 1)
#define TRITON_HOSTCAP_MSAA_RTV_FORCED_SC1       (1u << 2)
#define TRITON_HOSTCAP_EXTENDED_RESOURCE_SHARING (1u << 3)
#define TRITON_HOSTCAP_MAP_DEFAULT_BUFFERS       (1u << 4)
#define TRITON_HOSTCAP_SHADER_CACHE              (1u << 5)
/* Host D3D12 backend consumes DXIL containers (SM 6.x) directly. */
#define TRITON_HOSTCAP_DXIL                      (1u << 6)

/* Adapter-scope capability probe for DDI version selection (mirror of
 * the transport's npt_adapter_probe; caps_flags carries NPT_CAPSET_CAP_*
 * bits).  Runs a one-shot D3DKMT probe on first call and latches the
 * result process-wide. */
struct triton_adapter_probe {
   bool viogpu;
   bool wddm2;
   bool host_ok;
   uint32_t caps_flags;
   /* NPT_DEBUG policy bits, resolved here so the Triton TUs need not
    * reach the npt env machinery. */
   bool no_wddm2_ddi;
   bool wddm2_0_only;
};

void tritonSharedBridgeAdapterProbe(struct triton_adapter_probe *out);

/* Wire-neutral texture description; Triton converts to/from the WDDM
 * private-data struct (VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS). */
struct triton_shared_texture_desc {
   /* Filled by tritonSharedBridgeExportBlob. */
   uint64_t blob_id;
   uint32_t create_ctx_id;
   uint32_t plane_count;
   uint32_t texture_layout;
   uint64_t modifier;
   uint64_t allocation_size;
   struct {
      uint64_t offset;
      uint64_t pitch;
   } planes[TRITON_SHARED_MAX_PLANES];
   /* Owned by the caller (D3D11 description of the host texture). */
   uint32_t width;
   uint32_t height;
   uint32_t mip_levels;
   uint32_t array_size;
   uint32_t format;           /* DXGI_FORMAT */
   uint32_t sample_count;
   uint32_t usage;            /* D3D11_USAGE */
   uint32_t bind_flags;
   uint32_t cpu_access_flags;
   uint32_t misc_flags;
};

/* Exporter: export the host texture behind \p pResourceWrapper (an
 * ID3D11Texture2D* Neptune COM wrapper) as a dmabuf and stage it as
 * this context's pending blob.  Fills the export half of \p desc. */
bool tritonSharedBridgeExportBlob(void *pResourceWrapper,
                                  struct triton_shared_texture_desc *desc);

/* Consumer: attach the VM-global virtio resource \p res_id to this
 * process's transport context so the host worker receives its dmabuf
 * (Venus-style import).  Returns opaque WDDM handles for the release
 * call. */
bool tritonSharedBridgeImportRes(void *pDeviceWrapper, uint32_t res_id,
                                 uint64_t size, uint32_t *out_alloc,
                                 uint32_t *out_res_kmt);

void tritonSharedBridgeReleaseImportRes(void *pDeviceWrapper, uint32_t alloc,
                                        uint32_t res_kmt);

/* Consumer: import the texture backed by \p res_id on the device
 * behind \p pDeviceWrapper (an ID3D11Device* Neptune COM wrapper),
 * rebuilding it from \p desc, and return its ID3D11Texture2D* COM
 * wrapper (NULL on failure). */
void *tritonSharedBridgeOpenRes(void *pDeviceWrapper, uint32_t res_id,
                                const struct triton_shared_texture_desc *desc);

#ifdef __cplusplus
}
#endif

#endif /* TRITON_SHARED_BRIDGE_H */
