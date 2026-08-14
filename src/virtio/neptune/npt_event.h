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

/*
 * INVARIANT (stack-wide): any wait loop in this stack must exit on the
 * VALUE, never on the wake alone.  The wake (event/eventfd/KMD fence)
 * and the value (feedback slot / GetCompletedValue) travel independent
 * paths; a wake can be stale (recycled event, sibling gate, earlier
 * arm) and a value can arrive without its wake (lost SetEvent).  Every
 * waiter re-checks the value on wakeup and treats the wake as a hint.
 */

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

#if defined(_WIN32) || defined(__WINE__)
/* Per-thread cached auto-reset Win32 event (HANDLE) for guest-local
 * blocking waits (the SetEventOnCompletion(v, NULL) form).  Never
 * closed: an arm from a previous wait on this thread may still be
 * outstanding when the wait exits on the value, and its late firing
 * must hit a live handle we own, not a recycled one.  Callers drain
 * it (WaitForSingleObject(h, 0)) before each arm.  NULL on OOM. */
void *npt_event_thread_temp_event(void);

/* Fence hooks for npt_event_block_until_value. */
struct npt_event_block_ops {
   /* Cheap completion test: the feedback slot where that is trustworthy,
    * the authoritative read otherwise. */
   bool (*reached)(void *self, uint64_t value);
   /* Authoritative (synchronous) completion test, used periodically to
    * cross-check a wedged feedback publish path. */
   bool (*reached_sync)(void *self, uint64_t value);
   /* Ask the host to signal `token` once the fence reaches `value`. */
   void (*arm)(void *self, uint64_t value, uint64_t token);
   /* Names the interface in the stall log ("d3d11" / "d3d12"). */
   const char *name;
};

/* Block the calling thread until the fence reaches `value`: the
 * SetEventOnCompletion(value, NULL) form, served guest-side.  Never
 * forward that form to the host, which would service it with the
 * backend's own blocking SEOC(NULL) on the ring's dispatch thread --
 * head-of-line for every other guest thread on that ring, and a deadlock
 * when the Signal that satisfies it is queued behind the blocked ring
 * (unwedged only by TDR).  The arm is a wake hint; the value ends the
 * wait, per the invariant above. */
void npt_event_block_until_value(struct npt_device *dev,
                                 const struct npt_event_block_ops *ops,
                                 void *self, uint64_t value);
#endif

#endif /* NPT_EVENT_H */
