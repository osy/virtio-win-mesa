/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_env.h"

#include <stdlib.h>

#include "util/u_debug.h"
#include "util/os_misc.h"

struct npt_env npt_env;

static const struct debug_control npt_perf_options[] = {
   { "multi_ring",               NPT_PERF_MULTI_RING },
   { "no_fence_feedback",        NPT_PERF_NO_FENCE_FEEDBACK },
   { NULL, 0 },
};

static const struct debug_control npt_debug_options[] = {
   { "profile",                  NPT_DEBUG_PROFILE },
   { "force_vtest",              NPT_DEBUG_FORCE_VTEST },
   { "present_timing",           NPT_DEBUG_PRESENT_TIMING },
   { "expose_all_modes",         NPT_DEBUG_EXPOSE_ALL_MODES },
   { "no_wddm2_ddi",             NPT_DEBUG_NO_WDDM2_DDI },
   { "wddm2_0_only",             NPT_DEBUG_WDDM2_0_ONLY },
   { "d3d12_list_migration",     NPT_DEBUG_D3D12_LIST_MIGRATION },
   { "no_d3d12_xqueue_order",    NPT_DEBUG_NO_D3D12_XQUEUE_ORDER },
   { "d3d12_claim_atomic64",     NPT_DEBUG_D3D12_CLAIM_ATOMIC64 },
   { NULL, 0 },
};

static uint32_t
parse_clamped_u32(const char *name, uint32_t fallback,
                  uint32_t lo, uint32_t hi)
{
   const char *s = os_get_option(name);
   if (!s || !*s)
      return fallback;
   unsigned long v = strtoul(s, NULL, 10);
   return (v >= lo && v <= hi) ? (uint32_t)v : fallback;
}

static void
npt_env_init_impl(void)
{
   npt_env.perf =
      parse_debug_string(os_get_option("NPT_PERF"), npt_perf_options);
   npt_env.debug =
      parse_debug_string(os_get_option("NPT_DEBUG"), npt_debug_options);

   /* Profiler dump cadence; only consulted under NPT_DEBUG=profile. */
   npt_env.profile_period_ms =
      parse_clamped_u32("NPT_PROFILE_PERIOD_MS", 1000u, 1u, 60000u);

   /* Present-to-Present gap triggering a STUTTER log line.  Default
    * 50 ms (= the gap is < 20 fps).  */
   npt_env.stutter_threshold_ms =
      parse_clamped_u32("NPT_STUTTER_MS", 50u, 1u, 10000u);
}

static _Atomic int g_npt_env_init_state;

void
npt_env_init(void)
{
   NPT_CALL_ONCE(g_npt_env_init_state, npt_env_init_impl());
}

void
npt_env_force_perf(uint64_t perf_bits)
{
   npt_env_init();
   npt_env.perf |= perf_bits;
}
