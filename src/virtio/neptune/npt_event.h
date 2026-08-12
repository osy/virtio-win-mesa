/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest half of virglrenderer's npt_event.  Pipeline:
 *
 *   override -> npt_event_arm:
 *     REGISTER_EVENT + ARM_EVENT_FENCE + submit_present_fence;
 *     enqueues (sync_fd, hEvent) on the waiter
 *   -> generated thunk; host's _replace swaps in the proxy eventfd
 *   -> host SetEvent(eventfd) -> retire_fence
 *   -> waiter thread's poll() returns -> SetEvent(hEvent) ->
 *      RELEASE_EVENT
 *
 * Tokens are single-use: each arm mints its own, so each wait gets its
 * own host proxy, and the waiter releases it once that arm completes.
 * See npt_event_arm for why keying the proxy by HANDLE cannot work.
 */

#ifndef NPT_EVENT_H
#define NPT_EVENT_H

#include "npt_common.h"

struct npt_device;

void npt_event_init(struct npt_device *dev);
void npt_event_fini(struct npt_device *dev);

/* Call BEFORE the generated thunk so the host's _replace sees the
 * proxy eventfd.  Internally refcounted: the matching RELEASE is
 * driven by the waiter thread when the last in-flight arm completes. */
/* Arms a single-use host proxy for one wait on hEvent; returns the
 * minted token (nonzero) to embed in the wire call's HANDLE slot, or 0
 * on failure.  The waiter releases the token after completion. */
uint64_t npt_event_arm(struct npt_device *dev, void *hEvent);

/* Single-use token arm returning the caller-owned sync_file fd (-1 on
 * failure).  Pair a successful arm with npt_event_release_token after
 * the fd's wait completes; failures self-release.  The token must be
 * process-unique and never reused (host proxies are one-shot). */
int npt_event_arm_token_fd(struct npt_device *dev, uint64_t token);
void npt_event_release_token(struct npt_device *dev, uint64_t token);

/* Monitored-fence gate arm (see npt_event.c): arms a GATE_WAIT on the
 * drain fence reaching `value` and the matching KMD event-ring fence.
 * Returns the KMD gate token (0 on failure). */
uint64_t npt_event_gate_arm(struct npt_device *dev, uint64_t fence_obj_id,
                            uint64_t value);

#endif /* NPT_EVENT_H */
