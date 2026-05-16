/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Stubs for the native-Windows (-Dnpt_wine=false) build: neither
 * virtio-gpu DRM nor the vtest Unix-socket transport has a native
 * equivalent, so both backend factories return NULL and npt_device
 * fails cleanly.
 */

#include "npt_renderer.h"

struct npt_renderer *
npt_renderer_create_virtgpu(void)
{
   return NULL;
}

struct npt_renderer *
npt_renderer_create_vtest(void)
{
   return NULL;
}
