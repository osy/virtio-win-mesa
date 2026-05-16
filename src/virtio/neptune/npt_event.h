/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest half of virglrenderer's npt_event.  Pipeline:
 *
 *   override -> npt_event_arm:
 *     REGISTER_EVENT (first arm only) + ARM_EVENT_FENCE +
 *     submit_present_fence; enqueues (sync_fd, hEvent) on the waiter
 *   -> generated thunk; host's _replace swaps in the proxy eventfd
 *   -> host SetEvent(eventfd) -> retire_fence
 *   -> waiter thread's poll() returns -> SetEvent(hEvent) ->
 *      RELEASE_EVENT (when the per-HANDLE arm refcount drops to zero)
 *
 * Token = (uintptr_t)HANDLE; same HANDLE -> same proxy while at least
 * one arm is outstanding.  After the last arm completes the host
 * proxy is freed and the next arm re-REGISTERs.
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
bool npt_event_arm(struct npt_device *dev, void *hEvent);

#endif /* NPT_EVENT_H */
