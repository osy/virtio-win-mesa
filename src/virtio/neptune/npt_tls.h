/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-thread ring support.  Each thread gets its own shmem-backed
 * ring keyed via tss_t, destroyed on thread exit.  Two-stage teardown
 * so device-destroy can race thread-exit safely.
 */

#ifndef NPT_TLS_H
#define NPT_TLS_H

#include "npt_common.h"

struct npt_device;
struct npt_ring;

struct npt_tls {
   struct list_head tls_rings;
};

/*
 * Co-owned by TLS (tls_head) and device (dev_head).  First destroyer
 * tears down the ring; second finds tr->ring == NULL and frees the
 * struct.  ring is _Atomic so the get_ring fast path can skip the
 * mutex.
 */
struct npt_tls_ring {
   mtx_t mutex;
   _Atomic(struct npt_ring *) ring;
   struct npt_device *device;
   struct list_head tls_head;
   struct list_head dev_head;
};

/* Idempotent. */
void
npt_tls_once_init(void);

/* Falls back to dev->ring on TLS unavailable, ring-create failure,
 * or when multi-ring is disabled. */
struct npt_ring *
npt_tls_get_ring(struct npt_device *dev);

/* The calling thread's existing TLS ring for dev, or NULL when it has
 * none (never creates one; NULL when multi-ring is off).  For barriers
 * that only need to drain what this thread already submitted. */
struct npt_ring *
npt_tls_peek_ring(struct npt_device *dev);

/* Callable from thread exit (tss dtor) or device destroy. */
void
npt_tls_destroy_ring(struct npt_tls_ring *tr);

/*
 * Wait until the ring identified by `ring_id` (primary or any live
 * TLS ring) has dispatched past `seqno`.  A ring that no longer
 * exists was destroyed, and ring destroy is host-synchronous, so a
 * miss means the barrier is already satisfied.
 */
struct npt_ring_edge;

/* Snapshot the published tail of every ring of dev except skip_ring_id
 * into out[0..cap).  Returns the number of rings seen; when that exceeds
 * cap the caller retries with a larger buffer. */
uint32_t
npt_tls_snapshot_ring_tails(struct npt_device *dev, uint64_t skip_ring_id,
                            struct npt_ring_edge *out, uint32_t cap);

/* The published tail of ring_id, or false when no such ring exists any
 * more (its DESTROY_RING was host-synchronous, so its bytes are gone
 * from every ordering question). */
bool
npt_tls_ring_tail_by_id(struct npt_device *dev, uint64_t ring_id,
                        uint32_t *out_tail);

#endif /* NPT_TLS_H */
