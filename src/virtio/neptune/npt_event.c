/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <inttypes.h>
#include "npt_event.h"
#include "npt_com.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_renderer.h"
#include "npt_ring.h"
#include "nptunix/npt_unixlib.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#endif

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct event_pending {
   int    sync_fd;
   void  *handle;
   uint64_t token;     /* single-use host proxy token; released on completion */
   bool   direct;      /* KMD signals `handle` itself from its completion
                        * DPC; the waiter only releases the token */
   struct event_pending *next;
};

/* Upper bound on waiter threads.  Sized for the concurrent presents a
 * compositor drives through one device; past it arms queue rather than
 * spawn, since each extra thread only pays off while an arm is outstanding. */
#define NPT_EVENT_WAITER_MAX 8

/* Wait-slice granularity: bounds both shutdown latency at join and how
 * long a slow arm holds its pool thread before yielding to queued work. */
#define NPT_EVENT_WAIT_SLICE_MS 2000

struct npt_event_state {
   /* Half-open [base, end) carving the upper portion of the host's
    * sync_queues table; sized from the renderer's
    * max_timeline_count so we don't collide with Present rings.
    * See npt_renderer_event_ring_base in npt_renderer.h. */
   uint32_t                       ring_base;
   uint32_t                       ring_end;
   _Atomic uint32_t               next_event_ring_idx;

   /* Holds ARM + submit_present_fence back-to-back on the same ring. */
   mtx_t                          arm_mutex;

   /* Dedicated transport ring for the synchronous GATE_WAIT arms
    * (D3D12 DDI).  On the method ring the arm's reply queues behind
    * the recording flood and the host's ExecuteCommandLists, costing
    * milliseconds per arm on the submit thread; on this otherwise-empty
    * ring it is a bare wire round trip.  Safe because GATE_WAIT has no
    * FIFO dependency on
    * any other ring's traffic: it references only the long-created
    * drain fence object, and fence-vs-arm arrival order is already
    * handled by the host's pending-arm/parked-fence pairing.
    * Lazily created under arm_mutex; NULL until first gate arm. */
   struct npt_ring               *gate_ring;

   /* Arms waiting for a waiter thread, oldest first.  Each entry occupies a
    * waiter for as long as its fence takes to retire, so the queue is FIFO
    * and serviced by a pool: one thread would make every arm wait out the
    * one ahead of it, and the present gate arms once per frame per
    * swapchain, with several swapchains presenting concurrently. */
   mtx_t                          pending_mutex;
   cnd_t                          pending_cond;
   struct event_pending         *pending_head;
   struct event_pending         *pending_tail;
   uint32_t                       pending_count;
   _Atomic bool                   waiter_join;
   /* Threads alive, and of those the ones blocked in cnd_wait with no entry.
    * Both under pending_mutex; waiter_count is also the used length of
    * waiter_thread. */
   uint32_t                       waiter_count;
   uint32_t                       waiter_idle;
#if defined(_WIN32)
   HANDLE                         waiter_thread[NPT_EVENT_WAITER_MAX];
#else
   pthread_t                      waiter_thread[NPT_EVENT_WAITER_MAX];
#endif
};

#if defined(__WINE__)
static void
do_set_event(void *handle)
{
   if (handle)
      SetEvent((HANDLE)handle);
}
/* PE has no poll(); route through the unixlib. */
static int
wait_one(int fd, int timeout_ms)
{
   if (npt_unixlib_ensure_init() != 0)
      return -1;
   struct npt_unix_wait_fd_params p = {
      .fd = fd, .timeout_ms = timeout_ms, .result = -1,
   };
   if (npt_wine_unix_call(npt_unix_wait_fd, &p) != 0)
      return -1;
   return p.result;
}
static void
close_fd(int fd)
{
   if (fd < 0 || npt_unixlib_ensure_init() != 0)
      return;
   struct npt_unix_close_params p = { .fd = fd };
   npt_wine_unix_call(npt_unix_close, &p);
}
#elif defined(_WIN32)
static void
do_set_event(void *handle)
{
   if (handle)
      SetEvent((HANDLE)handle);
}
static int
wait_one(int fd, int timeout_ms)
{
   /* fd carries a Win32 auto-reset event HANDLE from
    * npt_vgw32_submit_present_fence; the KMD signals it from its completion DPC
    * when the host retires the event-ring fence, i.e. at GPU completion. */
   HANDLE h = (HANDLE)(ULONG_PTR)(ULONG)fd;
   DWORD rc = WaitForSingleObject(h, (DWORD)timeout_ms);
   return rc == WAIT_OBJECT_0 ? 1 : (rc == WAIT_TIMEOUT ? 0 : -1);
}
static void
close_fd(int fd)
{
   if (fd >= 0)
      CloseHandle((HANDLE)(ULONG_PTR)(ULONG)fd);
}
#else
static void
do_set_event(void *handle)
{
   (void)handle;
}
static int
wait_one(int fd, int timeout_ms)
{
   struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
   int ret;
   do { ret = poll(&pfd, 1, timeout_ms); } while (ret < 0 && errno == EINTR);
   return ret;
}
static void
close_fd(int fd)
{
   if (fd >= 0) close(fd);
}
#endif

#if defined(_WIN32) || defined(__WINE__)
/* See npt_event.h.  Deliberately never closed: a late-firing arm from a
 * previous wait must SetEvent a handle we still own -- CloseHandle
 * followed by kernel handle-value reuse would signal a stranger's
 * object.  One event per thread that ever blocks is a bounded leak. */
void *
npt_event_thread_temp_event(void)
{
   static NPT_TLS HANDLE tls_ev;
   if (!tls_ev)
      tls_ev = CreateEventW(NULL, FALSE /* auto-reset */, FALSE, NULL);
   return (void *)tls_ev;
}

/* Value-poll cadence for the blocking wait.  The armed wake is the fast
 * path (the host publishes the value before it signals); the slice only
 * bounds the cost of a lost wake or a failed arm. */
#define NPT_BLOCK_SLICE_MS 20
/* Slices between authoritative cross-checks, and between stall reports. */
#define NPT_BLOCK_SYNC_SLICES 50
#define NPT_BLOCK_STALL_SLICES 500

void
npt_event_block_until_value(struct npt_device *dev,
                            const struct npt_event_block_ops *ops,
                            void *self, uint64_t value)
{
   HANDLE ev = (HANDLE)npt_event_thread_temp_event();
   uint64_t token = 0;
   if (ev) {
      /* Drain a stale wake left by a previous wait on this thread whose
       * arm fired after that wait had already exited on the value. */
      WaitForSingleObject(ev, 0);
      token = npt_event_arm(dev, (void *)ev);
      if (token)
         ops->arm(self, value, token);
   }

   uint32_t slices = 0;
   while (!ops->reached(self, value)) {
      if (ev)
         WaitForSingleObject(ev, NPT_BLOCK_SLICE_MS);
      else
         npt_relax_sleep_us(1000);
      slices++;
      if (slices % NPT_BLOCK_SYNC_SLICES == 0 &&
          ops->reached_sync(self, value))
         return;
      if (slices % NPT_BLOCK_STALL_SLICES == 0) {
         npt_log("%s fence: SetEventOnCompletion(%" PRIu64 ", NULL) still "
                 "waiting after ~%us (armed=%d)", ops->name, value,
                 slices * NPT_BLOCK_SLICE_MS / 1000u, token != 0);
      }
   }
}
#endif

#if defined(_WIN32)
static DWORD WINAPI event_waiter_thread(LPVOID arg)
#else
static void *event_waiter_thread(void *arg)
#endif
{
   struct npt_device *dev = arg;
   struct npt_event_state *st = dev->event;

   for (;;) {
      mtx_lock(&st->pending_mutex);
      st->waiter_idle++;
      while (!st->pending_head && !atomic_load(&st->waiter_join))
         cnd_wait(&st->pending_cond, &st->pending_mutex);
      st->waiter_idle--;
      if (atomic_load(&st->waiter_join) && !st->pending_head) {
         mtx_unlock(&st->pending_mutex);
         break;
      }
      struct event_pending *p = st->pending_head;
      st->pending_head = p->next;
      if (!st->pending_head)
         st->pending_tail = NULL;
      st->pending_count--;
      mtx_unlock(&st->pending_mutex);

      /* Wait a slice at a time (each slice caps shutdown latency at
       * join).  An arm is never dropped on timeout -- a fence that
       * takes longer than one slice, or whose host-side signal is
       * delayed, would otherwise silently lose the SetEvent and hang
       * the app on its Win32 event.  But a slow arm must not
       * monopolize its pool thread while other arms are queued (the
       * pool is small and D3D12 apps leave many events armed for
       * later values): on timeout, if other work is waiting, re-queue
       * this arm to the tail and service the next, so the pool
       * round-robins instead of starving.  Only when this arm is the
       * sole outstanding one does the thread wait it out in place. */
      int rc = wait_one(p->sync_fd, NPT_EVENT_WAIT_SLICE_MS);
      while (rc == 0 && !atomic_load(&st->waiter_join)) {
         mtx_lock(&st->pending_mutex);
         bool others_waiting = st->pending_head != NULL;
         if (others_waiting) {
            p->next = NULL;
            if (st->pending_tail)
               st->pending_tail->next = p;
            else
               st->pending_head = p;
            st->pending_tail = p;
            st->pending_count++;
            /* Wake an idle peer so the re-queued arm keeps a servicer. */
            cnd_signal(&st->pending_cond);
            mtx_unlock(&st->pending_mutex);
            p = NULL;
            break;
         }
         mtx_unlock(&st->pending_mutex);
         rc = wait_one(p->sync_fd, NPT_EVENT_WAIT_SLICE_MS);
      }
      if (!p)
         continue; /* re-queued for a round-robin turn */

      if (rc > 0) {
         /* When the KMD signalled the app event directly from its DPC
          * this thread's only job left is the token release. */
         if (!p->direct)
            do_set_event(p->handle);
      } else {
         /* Never swallow the wake: a lost SetEvent is the worst fence
          * failure mode (the app parks on its event forever), while a
          * spurious early one is absorbed by the value-checked waiters
          * upstream (see the invariant in npt_event.h).  Signal on the
          * error/shutdown path too and log loudly. */
         npt_log("event: wait failed for handle %p (rc=%d%s) -- "
                 "signaling anyway", p->handle,
                 rc, rc == 0 ? ", shutdown" : "");
         do_set_event(p->handle);
      }
      close_fd(p->sync_fd);
      /* Single-use proxy: release after completion (the host firing
       * necessarily preceded the KMD signal we just consumed). */
      npt_dispatch_event_release_ring(npt_device_method_ring(dev), p->token);
      free(p);
   }
#if defined(_WIN32)
   return 0;
#else
   return NULL;
#endif
}

/* Add one thread to the pool.  Caller holds pending_mutex except at init,
 * where no other thread can see the state yet. */
static void
event_waiter_spawn(struct npt_device *dev, struct npt_event_state *st)
{
   if (st->waiter_count >= NPT_EVENT_WAITER_MAX)
      return;
#if defined(_WIN32)
   HANDLE t = CreateThread(NULL, 0, event_waiter_thread, dev, 0, NULL);
   if (!t)
      return;
   st->waiter_thread[st->waiter_count++] = t;
#else
   if (pthread_create(&st->waiter_thread[st->waiter_count], NULL,
                      event_waiter_thread, dev) != 0)
      return;
   st->waiter_count++;
#endif
}

void
npt_event_init(struct npt_device *dev)
{
   struct npt_event_state *st = calloc(1, sizeof(*st));
   if (!st) return;

   mtx_init(&st->arm_mutex, mtx_plain);
   mtx_init(&st->pending_mutex, mtx_plain);
   cnd_init(&st->pending_cond);
   const struct npt_renderer_info *info = &dev->renderer->info;
   st->ring_base = npt_renderer_event_ring_base(info);
   st->ring_end  = npt_renderer_event_ring_end(info);
   if (st->ring_end <= st->ring_base) {
      npt_log("event: max_timeline_count=%u too small to partition; "
              "Win32 events will share Present rings",
              info->max_timeline_count);
      st->ring_base = 1;
      st->ring_end  = info->max_timeline_count > 1
                      ? info->max_timeline_count : 2;
   }
   atomic_init(&st->next_event_ring_idx, st->ring_base);
   atomic_init(&st->waiter_join, false);
   st->pending_head = NULL;

   /* Set dev->event before thread create so the waiter can read it. */
   dev->event = st;

   event_waiter_spawn(dev, st);
}

void
npt_event_fini(struct npt_device *dev)
{
   struct npt_event_state *st = dev->event;
   if (!st) return;

   /* Wake every waiter so they observe the flag immediately.  The count is
    * stable here: only npt_event_arm grows the pool, and the device is being
    * destroyed. */
   mtx_lock(&st->pending_mutex);
   atomic_store(&st->waiter_join, true);
   cnd_broadcast(&st->pending_cond);
   const uint32_t waiters = st->waiter_count;
   mtx_unlock(&st->pending_mutex);

   for (uint32_t i = 0; i < waiters; i++) {
#if defined(_WIN32)
      WaitForSingleObject(st->waiter_thread[i], INFINITE);
      CloseHandle(st->waiter_thread[i]);
#else
      pthread_join(st->waiter_thread[i], NULL);
#endif
   }

   while (st->pending_head) {
      struct event_pending *p = st->pending_head;
      st->pending_head = p->next;
      close_fd(p->sync_fd);
      free(p);
   }
   st->pending_tail  = NULL;
   st->pending_count = 0;

   if (st->gate_ring) {
      mtx_lock(&dev->instance_rings_mutex);
      list_del(&st->gate_ring->instance_head);
      mtx_unlock(&dev->instance_rings_mutex);
      npt_ring_destroy(st->gate_ring);
      st->gate_ring = NULL;
   }

   cnd_destroy(&st->pending_cond);
   mtx_destroy(&st->pending_mutex);
   mtx_destroy(&st->arm_mutex);
   free(st);
   dev->event = NULL;
}

uint64_t
npt_event_arm(struct npt_device *dev, void *hEvent)
{
   if (!dev || !hEvent || !dev->event)
      return 0;

   struct npt_event_state *st = dev->event;

   /* SINGLE-USE token, minted per arm.  Keying the host proxy by the
    * raw HANDLE value reused one proxy pipe across every wait on the
    * same Win32 event -- and, as npt_event_arm_token_fd documents,
    * "the host eventfd proxy is never drained, so a signaled proxy
    * can't be re-armed for a second wait": after the first firing,
    * every later wait on that event completed instantly.  Apps reuse
    * one event across frames for fence pacing, so every such wait
    * observed the PREVIOUS operation's completion.  A fresh token
    * per arm gives each wait its own proxy; the waiter releases it
    * after the KMD-signalled completion, which the host firing
    * necessarily precedes. */
   uint64_t token = npt_com_allocate_next_id();
   npt_dispatch_event_register_ring(npt_device_method_ring(dev), token);

   /* Round-robin across event rings so independent in-flight arms
    * don't queue up on one sync-queue worker. */
   uint32_t ring_idx = atomic_fetch_add(&st->next_event_ring_idx, 1);
   const uint32_t span = st->ring_end - st->ring_base;
   ring_idx = st->ring_base + (ring_idx - st->ring_base) % span;

   /* Same dispatch thread as the SetEventOnCompletion that follows
    * and the DC::Signal that triggers the fence.  Pre-DC fallback
    * to primary is safe (nothing's racing yet). */
   mtx_lock(&st->arm_mutex);
   bool arm_ok = npt_dispatch_event_arm_fence(npt_device_method_ring(dev),
                                              token, ring_idx);
   if (!arm_ok) {
      mtx_unlock(&st->arm_mutex);
      npt_dispatch_event_release_ring(npt_device_method_ring(dev), token);
      return 0;
   }
   /* Hand the app's own event to the transport so the KMD can
    * KeSetEvent it straight from the completion DPC; the waiter then
    * only does token bookkeeping for that arm. */
   bool direct_ok = false;
   int sync_fd = npt_renderer_submit_present_fence_direct(dev->renderer,
                                                          ring_idx, hEvent,
                                                          &direct_ok);
   mtx_unlock(&st->arm_mutex);

   if (sync_fd < 0) {
      npt_dispatch_event_release_ring(npt_device_method_ring(dev), token);
      return 0;
   }

   struct event_pending *p = calloc(1, sizeof(*p));
   if (!p) {
      close_fd(sync_fd);
      npt_dispatch_event_release_ring(npt_device_method_ring(dev), token);
      return 0;
   }
   p->sync_fd = sync_fd;
   p->handle  = hEvent;
   p->token   = token;
   p->direct  = direct_ok;

   mtx_lock(&st->pending_mutex);
   p->next = NULL;
   if (st->pending_tail)
      st->pending_tail->next = p;
   else
      st->pending_head = p;
   st->pending_tail = p;
   st->pending_count++;
   /* Grow the pool when the queue outruns the waiters still unclaimed: an
    * idle waiter that has been signalled but not yet woken is still counted
    * idle, so comparing against the depth avoids piling a second arm onto it.
    * Each arm gates a present and holds its waiter until the fence retires. */
   if (st->pending_count > st->waiter_idle)
      event_waiter_spawn(dev, st);
   cnd_signal(&st->pending_cond);
   mtx_unlock(&st->pending_mutex);
   return token;
}

/*
 * Single-use token arm for callers that consume the sync_file
 * themselves (the guest swapchain's per-Present GPU-done fence)
 * instead of routing through the waiter thread.  Shares the ring
 * allocator and arm_mutex with npt_event_arm so ARM +
 * submit_present_fence pairs from both users can never cross-match on
 * a ring's host-side FIFO.
 *
 * The caller owns the returned fd (or -1) and must pair a successful
 * arm with npt_event_release_token once the wait is over -- tokens
 * are single-use: the host eventfd proxy is never drained, so a
 * signaled proxy can't be re-armed for a second wait.
 */
int
npt_event_arm_token_fd(struct npt_device *dev, uint64_t token)
{
   struct npt_event_state *st = dev->event;
   if (!st || !token)
      return -1;

   npt_dispatch_event_register(dev->renderer, token);

   uint32_t ring_idx = atomic_fetch_add(&st->next_event_ring_idx, 1);
   /* npt_event_init repairs a degenerate range, so the span is never zero --
    * npt_event_arm relies on the same invariant.  Asserting it keeps the
    * registration above paired with a release on every reachable exit. */
   const uint32_t span = st->ring_end - st->ring_base;
   assert(span);
   ring_idx = st->ring_base + (ring_idx - st->ring_base) % span;

   mtx_lock(&st->arm_mutex);
   bool arm_ok = npt_dispatch_event_arm_fence(npt_device_method_ring(dev),
                                              token, ring_idx);
   int sync_fd = arm_ok
      ? npt_renderer_submit_present_fence(dev->renderer, ring_idx) : -1;
   mtx_unlock(&st->arm_mutex);

   if (sync_fd < 0)
      npt_dispatch_event_release(dev->renderer, token);
   return sync_fd;
}

void
npt_event_release_token(struct npt_device *dev, uint64_t token)
{
   if (token)
      npt_dispatch_event_release(dev->renderer, token);
}

/*
 * Monitored-fence gate arm (D3D12 DDI): pair a host event-ring fence
 * with a KMD gate token.  The KMD's VIOGPU_ARM_GATE escape submits the
 * empty fenced SUBMIT_3D and fires the returned token when the host
 * retires the fence at real GPU completion; a DMA packet on the D3D12
 * queue's kernel context (VIOGPU_CMD_GATE{token}) parks on it, which is
 * what gates dxgkrnl's monitored-fence packets.
 *
 * The host side is a GATE_WAIT: SetEventOnCompletion(drain fence, value)
 * onto the ring's PERSISTENT gate eventfd, with value-checked
 * retirement (GetCompletedValue >= value re-verified on every wakeup).
 * Per-arm single-use eventfds were unsound: the host D3D library fires
 * callbacks asynchronously by NUMERIC fd, and fd reuse after retirement
 * lets a late fire signal the NEXT gate, which reads back stale by one.
 *
 * Returns the KMD gate token (0 on failure).
 */
/* Transport ring for GATE_WAIT (see gate_ring in npt_event_state).
 * Called under arm_mutex.  Falls back to the method ring if creation
 * fails. */
static struct npt_ring *
event_gate_transport_ring(struct npt_device *dev, struct npt_event_state *st)
{
   if (st->gate_ring)
      return st->gate_ring;
   struct npt_ring_layout layout;
   npt_ring_get_layout(16u * 1024u, sizeof(uint32_t), &layout);
   const uint64_t ring_id = atomic_fetch_add(&dev->next_ring_id, 1);
   struct npt_ring *ring =
      npt_ring_create(dev, &layout, ring_id, false /* is_tls_ring */);
   if (!ring)
      return npt_device_method_ring(dev);
   mtx_lock(&dev->instance_rings_mutex);
   list_addtail(&ring->instance_head, &dev->instance_rings);
   mtx_unlock(&dev->instance_rings_mutex);
   st->gate_ring = ring;
   return ring;
}

uint64_t
npt_event_gate_arm(struct npt_device *dev, uint64_t fence_obj_id,
                   uint64_t value)
{
   if (!dev || !dev->event)
      return 0;
   struct npt_event_state *st = dev->event;

   uint32_t ring_idx = atomic_fetch_add(&st->next_event_ring_idx, 1);
   const uint32_t span = st->ring_end - st->ring_base;
   assert(span);
   ring_idx = st->ring_base + (ring_idx - st->ring_base) % span;

   mtx_lock(&st->arm_mutex);
   /* Escape first: if the fence outruns the GATE_WAIT the host parks it
    * (the pairing handles either order).  The reverse order with a
    * failing escape would leave an orphaned pending-arm that mispairs
    * with this ring's next fence. */
   uint64_t kmd_token = npt_renderer_arm_gate_fence(dev->renderer, ring_idx);
   bool arm_ok = kmd_token != 0 &&
      npt_dispatch_event_gate_wait(event_gate_transport_ring(dev, st),
                                   fence_obj_id, value, ring_idx);
   mtx_unlock(&st->arm_mutex);

   if (!kmd_token)
      return 0;
   if (!arm_ok) {
      /* The KMD fence is in flight with no arm: it parks host-side and
       * would pair off-by-one with this ring's next arm.  A GATE_WAIT
       * failure means the method ring is already fatal; log loud and
       * let the caller skip the gate (TDR recovery applies if the ring
       * truly died). */
      npt_log("event: GATE_WAIT failed after escape (ring=%u) -- "
              "ring fatal?", ring_idx);
      return 0;
   }
   return kmd_token;
}
