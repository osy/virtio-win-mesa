/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-side ring buffer.  Single-producer (guest writes tail) /
 * single-consumer (host writes head) circular buffer over a shmem
 * blob.  Used for the primary ring, per-instance (DC, swapchain)
 * rings, and TLS rings.
 */

#ifndef NPT_RING_H
#define NPT_RING_H

#include "npt_common.h"
#include "npt_cs.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "npt_shmem_pool.h"
#include "npt_transport_defs.h"

#include "util/futex.h"

/*
 * Shmem layout (64-byte aligned):
 *   0:   head (host writes, guest reads)
 *   64:  tail (guest writes, host reads)
 *   128: status (host writes, guest reads; NPT_RING_STATUS_*_BIT)
 *   192: circular buffer
 *   192 + buffer_size: extra region
 */

/*
 * Ring lock: bounded test-and-test-and-set spin, then futex park.
 *
 * The common critical section is tens of instructions (space check +
 * two bounded memcpys + tail publish) and every recording thread takes
 * the lock for every wire command -- hundreds of thousands of
 * acquisitions per second.  A kernel-arbitrated mutex convoys at that
 * rate: each contended handoff parks and wakes a thread, and under x64
 * emulation that cost dominates the submit path (measured on CP2077:
 * per-submit 600 ns at 4 vCPUs -> 2.6 us at 8 vCPUs, fps 54 -> 14).
 * The spin phase resolves those handoffs in user space.
 *
 * The lock is NOT bounded-hold, so spinning cannot be the last resort:
 * the holder can sit in npt_ring_wait_space's escalating-sleep backoff
 * (ring full), an upload-drain or virtqueue-seqno wait, or be
 * descheduled (vCPU preemption).  Guest vCPUs share host cores with
 * the render server, so unbounded spinning steals CPU from the very
 * consumer the holder is waiting on.  Past the spin budget, waiters
 * park on the lock word (futex; WaitOnAddress on Windows) and cost
 * nothing until release.
 *
 * States: 0 free, 1 held, 2 held with possible waiters (Drepper
 * "mutex3").  Release wakes one waiter only from state 2, so the
 * uncontended and spin-resolved paths never enter the kernel.
 *
 * The word lives in the heap-allocated npt_ring, not the WC-mapped
 * shmem, so atomic RMWs on it are safe on every arch (see the
 * npt_wc_atomic_ok discussion in npt_ring.c).
 */
struct npt_ring_lock {
   atomic_uint v;
};

#define NPT_RING_LOCK_YIELD() thrd_yield()
#if defined(_WIN32)
#  define NPT_RING_LOCK_RELAX() YieldProcessor()
#elif defined(__x86_64__) || defined(__i386__)
#  define NPT_RING_LOCK_RELAX() __builtin_ia32_pause()
#elif defined(__aarch64__)
#  define NPT_RING_LOCK_RELAX() __asm__ __volatile__("isb" ::: "memory")
#else
#  define NPT_RING_LOCK_RELAX() ((void)0)
#endif

static inline void
npt_ring_lock_init(struct npt_ring_lock *l)
{
   atomic_store_explicit(&l->v, 0u, memory_order_relaxed);
}

static inline bool
npt_ring_lock_try(struct npt_ring_lock *l)
{
   unsigned expected = 0u;
   return atomic_compare_exchange_weak_explicit(&l->v, &expected, 1u,
                                                memory_order_acquire,
                                                memory_order_relaxed);
}

static inline void
npt_ring_lock_acquire(struct npt_ring_lock *l)
{
   if (likely(npt_ring_lock_try(l)))
      return;

   /* Spin budget sized for several queued short handoffs; the yield
    * between rounds gives an off-cpu holder a slice to finish. */
   for (unsigned round = 0; round < 4u; round++) {
      for (unsigned spins = 0; spins < 128u; spins++) {
         if (atomic_load_explicit(&l->v, memory_order_relaxed) == 0u &&
             npt_ring_lock_try(l))
            return;
         NPT_RING_LOCK_RELAX();
      }
      NPT_RING_LOCK_YIELD();
   }

#if UTIL_FUTEX_SUPPORTED
   /* Park: advertise waiters with state 2, sleep while it stays 2.
    * The xchg on wake re-asserts 2 because this waiter cannot know it
    * was the last; the cost is one spurious wake on the final release
    * of a contended episode. */
   while (atomic_exchange_explicit(&l->v, 2u, memory_order_acquire) != 0u)
      futex_wait((uint32_t *)(void *)&l->v, 2, NULL);
#else
   for (;;) {
      while (atomic_load_explicit(&l->v, memory_order_relaxed) != 0u)
         NPT_RING_LOCK_YIELD();
      if (npt_ring_lock_try(l))
         return;
   }
#endif
}

static inline void
npt_ring_lock_release(struct npt_ring_lock *l)
{
#if UTIL_FUTEX_SUPPORTED
   if (unlikely(atomic_exchange_explicit(&l->v, 0u,
                                         memory_order_release) == 2u))
      futex_wake((uint32_t *)(void *)&l->v, 1);
#else
   atomic_store_explicit(&l->v, 0u, memory_order_release);
#endif
}

/*
 * Per-thread staging (single-ring model).
 *
 * Submitting a command straight into the ring costs ring->lock plus a
 * tail publish (store-buffer drain, tail store, full fence, status
 * load): ~300-400 ns and one lock hand-off between every recording
 * thread, per command.  A command whose effects other threads can only
 * observe through a later ordering point -- D3D12 list recording, CPU
 * descriptor writes (registry: staged) -- is instead appended to the
 * calling thread's staging buffer with no shared state touched, and the
 * buffer is copied into the ring under the lock as one burst.  The
 * wire format and the host are unchanged: the host sees the same bytes
 * arrive in bursts.  Multi-ring, which gives each thread its own ring
 * and host decoder, does not stage.
 *
 * What publishes a stage:
 *  - the owner, before every immediate submit it makes (program order),
 *    when the buffer is full, and at thread exit;
 *  - ExecuteCommandLists, which flushes every stage (the FIFO a single
 *    ring gives at that point: the submission consumes descriptor
 *    writes and recordings from every thread);
 *  - a thread continuing a list another thread recorded
 *    (npt_com_self_ring), and the descriptor-slot tracking in
 *    npt_overrides_d3d12_device.c, which publish the other thread's
 *    stage before appending a dependent command.
 * Everything else a thread submits is immediate and goes through
 * ring->lock as before, after its own stage.
 *
 * A stage is owned by one thread; its lock is taken by the owner for
 * every append and by other threads only to force a flush, so it is
 * uncontended in the common case.  Lock order is stage -> ring; no
 * thread holds its own stage lock while taking another's.
 */
#define NPT_RING_STAGE_SIZE (16u * 1024u)
/* Bounded by the reader masks in the descriptor tracking (one bit per
 * stage); a thread past the limit submits immediately. */
#define NPT_RING_STAGE_MAX  64u

struct npt_ring_stage {
   struct npt_ring_lock lock;
   /* NULL once the ring is destroyed; the owner then frees the stage at
    * exit.  Guarded by lock. */
   struct npt_ring *ring;
   uint32_t index;
   /* Bytes staged; read racily by other threads to skip empty stages. */
   _Atomic uint32_t used;
   /* Flushes completed.  A command appended while epoch == e is
    * published by flush e + 1.  Never rewinds, across adoption too. */
   _Atomic uint64_t epoch;
   /* Cleared by the owning thread at exit; a later thread adopts the
    * stage.  Guarded by lock. */
   bool live;
   /* The owner's latest append (npt_ring_stage_ref); owner-private. */
   uint64_t last_ref;
   uint8_t buf[NPT_RING_STAGE_SIZE];
};

/* Names one staged append: (index + 1) << 48 | epoch at the append.
 * 0 = nothing staged (the command went straight to the ring). */
typedef uint64_t npt_ring_stage_ref;

static inline uint32_t
npt_ring_stage_ref_index(npt_ring_stage_ref ref)
{
   return (uint32_t)(ref >> 48);
}

struct npt_ring_layout {
   size_t head_offset;
   size_t tail_offset;
   size_t status_offset;

   size_t buffer_offset;
   size_t buffer_size;

   size_t extra_offset;
   size_t extra_size;

   size_t shmem_size;
};

struct npt_device;

#define NPT_RING_EDGE_SEEN_SLOTS 8u

/* A peer ring's published position, as snapshotted for an ordering
 * edge. */
struct npt_ring_edge {
   uint64_t ring_id;
   uint32_t seqno;
};

struct npt_ring {
   uint64_t id;
   struct npt_renderer *renderer;
   /* Backpointer used by release-batch flush (cross-ring drain) and
    * by TLS rings to find primary on teardown. */
   struct npt_device *device;
   bool is_tls_ring;
   /* Linked into dev->instance_rings for DC / SC wrappers; empty
    * otherwise. */
   struct list_head instance_head;
   struct npt_renderer_shmem *shmem;

   uint32_t buffer_size;
   uint32_t buffer_mask;

   const volatile atomic_uint *head;
   volatile atomic_uint *tail;
   volatile atomic_uint *status;
   /* Host-written once at ring create: the backend workaround flags word
    * (NPT_WA_* | NPT_WA_FLAGS_PRESENT). Read by npt_host_workaround_flags(). */
   const volatile atomic_uint *wa_word;
   void *buffer;
   void *extra;

   uint32_t cur;

   struct npt_ring_lock lock;

   /* Rate-limit idle notifications: only notify if
    * NPT_RING_IDLE_TIMEOUT_NS elapsed since the last one, to avoid
    * EXECBUFFER round-trips when the host is briefly idle. */
   int64_t last_notify;
   int64_t next_notify;

   /* Set under lock when a submit finds the host IDLE; the doorbell
    * itself (a kernel escape through the renderer) fires after the
    * lock is dropped.  Lock-held waits on host progress must flush it
    * first or a parked host never wakes to make that progress. */
   bool notify_pending;

   /* Bump allocator for per-call reply windows.  Each sync submission
    * carves a region and SET_REPLY_STREAMs it just before the command,
    * so concurrent sync calls don't fight over a fixed slot. */
   struct npt_shmem_pool reply_pool;

   /* Indirect ("upload") shmem: commands larger than direct_size are
    * copied here and referenced by EXECUTE_COMMAND_STREAM (res_id,
    * offset, size).  Keeps the ring small while allowing multi-MiB
    * RESOURCE_UPDATE.  Doubles on demand; resets to offset 0 once the
    * host has caught up past upload_horizon_seqno. */
   struct npt_renderer_shmem *upload_shmem;
   uint32_t upload_size;
   uint32_t upload_used;
   uint32_t upload_horizon_seqno;

   /* buffer_size >> NPT_RING_DIRECT_ORDER. */
   uint32_t direct_size;

   /* Bumped each time npt_ring_relax sees ALIVE cleared; reset on
    * ALIVE set.  abort()s if it hits the threshold so a wedged host
    * fails loudly instead of spinning forever. */
   uint32_t watchdog_misses;

   /* Virtqueue-seqno roundtrip counter.  Minted outside ring->lock, so
    * seqnos can enter the ring out of order; the host stores the maximum
    * and a wait for an older seqno is already satisfied by a newer one. */
   _Atomic uint64_t roundtrip_next;
   /* Highest peer seqno this ring has already been ordered after, per
    * peer ring: an edge at or below it is redundant (a decode position
    * only advances).  Written under ring->lock; read without it by the
    * covered-check, where a stale entry only costs a redundant edge. */
   struct {
      _Atomic uint64_t ring_id;
      _Atomic uint32_t seqno;
   } edge_seen[NPT_RING_EDGE_SEEN_SLOTS];
   uint32_t edge_seen_next;

   struct npt_profile_ring profile;

   /* Global list of live rings for the profile dump walker. */
   struct list_head profile_head;

   /* Per-thread staging (see "Per-thread staging" above).  `staging` is
    * set on the primary ring of a single-ring device.  stages[] holds
    * every stage created for this ring, at its index; it only grows
    * (entries and stage_count are published with release, read with
    * acquire) and stages_mutex guards the growth. */
   bool staging;
   mtx_t stages_mutex;
   _Atomic uint32_t stage_count;
   _Atomic(struct npt_ring_stage *) stages[NPT_RING_STAGE_MAX];
};

void
npt_ring_get_layout(size_t buf_size, size_t extra_size,
                    struct npt_ring_layout *layout);

struct npt_ring *
npt_ring_create(struct npt_device *device,
                const struct npt_ring_layout *layout,
                uint64_t ring_id,
                bool is_tls_ring);

void
npt_ring_destroy(struct npt_ring *ring);


/* Used for transport commands that must stay ordered with ring
 * commands (notably RESOURCE_UPDATE, which must precede any Draw
 * that reads the buffer); going via submit_cmd would race the ring
 * thread on a different host thread. */
bool
npt_ring_submit_raw(struct npt_ring *ring, const void *data, uint32_t size);

/* `out_seqno` is the value `head` must reach for the bytes to be
 * fully consumed -- the N-slot rename ring uses it to decide when a
 * shmem slot is safe to reuse. */
bool
npt_ring_submit_raw_seqno(struct npt_ring *ring, const void *data,
                          uint32_t size, uint32_t *out_seqno);

/* Submit [header || payload || padding] without forcing the caller to
 * first coalesce into one malloc'd buffer.  Reserves
 * header_size + payload_padded_size bytes in the ring (so the host
 * parser advances correctly), but only memcpy's payload_size bytes
 * from `payload` -- padding tail bytes are left as whatever was
 * already in the ring/upload_shmem.  Used by RESOURCE_UPDATE where
 * payload_size can be multi-MB; saves a malloc + memcpy per
 * subresource on texture initial-data uploads. */
bool
npt_ring_submit_raw_with_payload(struct npt_ring *ring,
                                 const void *header, uint32_t header_size,
                                 const void *payload, uint32_t payload_size,
                                 uint32_t payload_padded_size);

/*
 * Force a virtqueue-seqno roundtrip: SUBMIT via the renderer's sync
 * path + matching WAIT through the ring so the ring thread blocks
 * until the host's virtqueue dispatcher catches up.  Required before
 * any ring command that names a freshly-created shmem's res_id
 * (e.g. REGISTER_QUERY_FEEDBACK), otherwise the ring thread races
 * the resource-table register.
 *
 * A no-op on transports whose shmem create is already host-visible on
 * return (npt_renderer::shmem_create_host_visible).
 */
void
npt_ring_force_roundtrip(struct npt_ring *ring);

/* Yield-heavy poll until head is past seqno.  Returns the relax-
 * iteration count (sync-reply path feeds it to the profiler).
 * Callers may ignore the return value. */
uint32_t
npt_ring_wait_seqno(struct npt_ring *ring, uint32_t seqno);

/* The seqno `head` must reach for everything published on this ring so
 * far to have been decoded.  Reads the shared tail, so it is safe to use
 * as a peer edge target: every byte below it is already visible to the
 * host.  A value racing a concurrent submitter names a later point in
 * the same stream, never an earlier one. */
static inline uint32_t
npt_ring_seqno_now(const struct npt_ring *ring)
{
   return atomic_load_explicit(ring->tail, memory_order_acquire);
}

/* Cross-ring ordering.  Every routine below places a WAIT_PEER edge on
 * `ring` so the HOST decodes nothing further on it until the named peer
 * ring has decoded past the named seqno; the calling guest thread never
 * blocks.  Targets are always positions already published on the peer
 * (its tail, or the seqno a completed submit returned), which keeps
 * the host's waits satisfiable and the edge graph acyclic.  A target at
 * or below the position `ring` was last ordered after is dropped. */

/* Order `ring` after peer `target_id` reaching `target_seqno`.  No-op
 * when target_id is `ring` itself. */
void
npt_ring_order_after(struct npt_ring *ring, uint64_t target_id,
                     uint32_t target_seqno);

/* Same, naming the peer by pointer (for Triton, which cannot see the
 * struct). */
void
npt_ring_order_after_ring(struct npt_ring *ring, const struct npt_ring *target,
                          uint32_t target_seqno);

/* Order `ring` after everything currently published on every other ring
 * of its device -- the single-ring FIFO guarantee at this point in the
 * caller's program order, without the single ring. */
void
npt_ring_order_after_all(struct npt_ring *ring);

/* Order `ring` after each listed edge. */
void
npt_ring_order_after_edges(struct npt_ring *ring,
                           const struct npt_ring_edge *edges, uint32_t count);

/* The calling thread's stage index + 1 on `ring`, or 0 when it has none
 * (staging off on the ring, or more than NPT_RING_STAGE_MAX threads). */
uint32_t
npt_ring_stage_self_index(struct npt_ring *ring);

/* The calling thread's latest staged append on `ring`, or 0. */
npt_ring_stage_ref
npt_ring_stage_last_ref(struct npt_ring *ring);

/* Publish the append `ref` names if it is still staged.  A ref on the
 * calling thread's own stage needs nothing: program order covers it. */
void
npt_ring_stage_ensure_flushed(struct npt_ring *ring, npt_ring_stage_ref ref);

/* Publish everything stage `index` (1-based) holds. */
void
npt_ring_stage_flush_index(struct npt_ring *ring, uint32_t index);

/* Publish everything the stages in `mask` hold (bit i = index i + 1). */
void
npt_ring_stage_flush_mask(struct npt_ring *ring, uint64_t mask);

/* Publish everything every stage of `ring` holds, the caller's included. */
void
npt_ring_stage_flush_all(struct npt_ring *ring);

/* Turn staging off for good, publishing everything first.  For a device
 * that switches to the multi-ring model after the ring exists. */
void
npt_ring_stage_disable(struct npt_ring *ring);

/*
 * Queue a COM_RELEASE for host_id on the calling thread's ring (primary
 * when the thread has none).  Ring FIFO orders it after this thread's
 * own uses of the object; the host defers the release itself until
 * every other ring has decoded past what was published when the
 * release arrived, so no guest-side drain is needed.
 */
void
npt_ring_send_com_release(struct npt_device *dev, uint64_t host_id);

#endif /* NPT_RING_H */
