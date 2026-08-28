/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D11Buffer overrides: GetDesc answers from the create-time desc in
 * the aux (falling back to the wire for wrappers minted outside
 * CreateBuffer); Map/Unmap go through the immediate-context override.
 * Also registers aux_size / aux_init for the runtime.
 */

#include "npt_com.h"
#include "npt_overrides.h"
#include "npt_resource.h"

#include "neptune-protocol/npt_protocol_client_id3d11buffer.h"
#include "neptune-protocol/npt_protocol_defs.h"

static const GUID *const buffer_tiers[] = { &NPT_IID_ID3D11Buffer, NULL };

static void NPT_STDMETHODCALLTYPE
buf_GetDesc_override(void *self, D3D11_BUFFER_DESC *pDesc)
{
   if (!pDesc) return;
   struct npt_d3d11_buffer *b = npt_d3d11_buffer_cast(self);
   if (b && npt_d3d11_buffer_fill_desc(b, pDesc))
      return;
   npt_id3d11buffer_default_GetDesc(self, pDesc);
}

void
npt_overrides_d3d11_buffer_init(void)
{
   NPT_REGISTER_OVERRIDE(id3d11buffer, GetDesc, buf_GetDesc_override);
   npt_com_register_family(buffer_tiers,
                           sizeof(struct npt_d3d11_buffer_aux),
                           npt_d3d11_buffer_aux_init);
}
