/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * NPT_PERF / NPT_DEBUG hold comma-separated tokens, each toggling
 * one bit in the matching bitmask.  Perf tokens follow the "no_*"
 * opt-out convention (everything on by default; the env is a
 * bisection killswitch).  Add a new flag by extending the enum here
 * and the option table in npt_env.c.
 */

#ifndef NPT_ENV_H
#define NPT_ENV_H

#include <stdint.h>

#include "npt_common.h"
#include "util/macros.h"

enum npt_perf {
   /* Opt-in: multi-ring on D3D11 is not the default. */
   NPT_PERF_MULTI_RING               = 1ull << 0,

   NPT_PERF_NO_FENCE_FEEDBACK        = 1ull << 1,
};

enum npt_debug {
   NPT_DEBUG_PROFILE                 = 1ull << 0,

   /* Skip the virtgpu probe in npt_device_create so the guest falls
    * through to the vtest backend even when /dev/dri/renderDxxx is
    * accessible. */
   NPT_DEBUG_FORCE_VTEST             = 1ull << 1,

   /* Per-Present timing breakdown. */
   NPT_DEBUG_PRESENT_TIMING          = 1ull << 2,

   /* Report the connector's full KMS mode list through IDXGIOutput
    * instead of capping it to the active CRTC scanout.  For modeset
    * experiments where the guest is meant to drive resolution changes. */
   NPT_DEBUG_EXPOSE_ALL_MODES        = 1ull << 3,

   /* Cap the D3D11 DDI advertisement at D3D11.1 regardless of KMD mode. */
   NPT_DEBUG_NO_WDDM2_DDI            = 1ull << 4,

   /* Cap the D3D11 DDI advertisement at D3DWDDM2_0 (drop 2_1/2_2).
    * Triage lever: separates "new-tier DDI table bug" from "cap bug"
    * without a rebuild. */
   NPT_DEBUG_WDDM2_0_ONLY            = 1ull << 5,

   /* Report D3D12 command lists whose recording migrates between threads.
    * Under NPT_PERF=multi_ring that splits one list's recording across
    * TLS rings, so the host decodes it out of order. */
   NPT_DEBUG_D3D12_LIST_MIGRATION    = 1ull << 6,

   /* Drop the cross-queue submission ordering the D3D12 queues impose on
    * each other, restoring the host's concurrent execution of independent
    * queues.  Recovers the async-compute overlap at the cost of the
    * ordering an app's unobservable Wait() would have given it. */
   NPT_DEBUG_NO_D3D12_XQUEUE_ORDER   = 1ull << 7,

   /* Report the host's 64-bit atomic caps (OPTIONS9/11) even though the
    * host executes only InterlockedMin/Max on typed and raw buffers:
    * Add/And/Or/Xor/Exchange/CompareExchange drop the whole dispatch and
    * group-shared 64-bit atomics read back 0 (d3d12-atomic64-test).
    * Opt-in for engines whose atomic64 use is the min/max subset (UE5
    * Nanite / virtual shadow maps). */
   NPT_DEBUG_D3D12_CLAIM_ATOMIC64    = 1ull << 8,
};

struct npt_env {
   uint64_t perf;
   uint64_t debug;
   /* Scalar tuning knobs (parsed once in npt_env_init).  These are
    * runtime-tunable values that don't fit the NPT_PERF/NPT_DEBUG
    * bitmask, but live here so every env-var read is in one place. */
   uint32_t profile_period_ms;     /* NPT_PROFILE_PERIOD_MS */
   uint32_t stutter_threshold_ms;  /* NPT_STUTTER_MS */
   /* Present-gate wait breakdown (tritonPresentFlushAndGate): log any
    * gate slower than this many ms, split into lock/wait halves with
    * per-wake obj0-vs-timeout classification.  0 = off.  The companion
    * to the 1s deadline canary: when "present-fence: GPU wait timed
    * out" fires, this is the tool that says which half lost the time. */
   uint32_t present_gate_trace_ms; /* NPT_PRESENT_GATE_TRACE */
   /* Event-waiter pool latency (npt_event.c): log any arm whose
    * enqueue-to-completion exceeds this many ms, split into queued_us
    * (waiting for a pool thread -- head-of-line pressure) and wait_us
    * (the fence itself).  0 = off. */
   uint32_t event_waiter_trace_ms; /* NPT_EVENT_WAITER_TRACE */
};

extern struct npt_env npt_env;

void npt_env_init(void);

/* unlikely(): "perf disabled" is the rare bisect path. */
#define NPT_PERF(category)  (unlikely(npt_env.perf & NPT_PERF_##category))
#define NPT_DEBUG(category) (unlikely(npt_env.debug & NPT_DEBUG_##category))

#endif /* NPT_ENV_H */
