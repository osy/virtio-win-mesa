/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_tls.h"
#include "npt_device.h"
#include "npt_ring.h"

#include <stdlib.h>

/* Per-thread recording ring.  16 KiB was enough for D3D11, but D3D12
 * command-list recording writes far more per draw (root descriptor
 * tables, PSO/state, indirect args) and profiling shows recording
 * threads spending a third of their driver time in npt_ring_wait_space,
 * blocked on the host draining this ring.  Bigger rings only absorb
 * bursts -- the host drain rate is the steady-state limit. */
#define NPT_TLS_RING_BUFFER_SIZE_DEFAULT (256u * 1024u)

static unsigned
npt_tls_ring_buffer_size(void)
{
   return NPT_TLS_RING_BUFFER_SIZE_DEFAULT;
}

/* Per-ring extra region. */
#define NPT_TLS_RING_EXTRA_SIZE sizeof(uint32_t)

static tss_t npt_tls_key;
/* 0=uninit, 1=initing, 2=done, 3=failed-stay-off */
static _Atomic int npt_tls_init_state;

static void
npt_tls_free(void *data)
{
   struct npt_tls *tls = data;
   if (!tls)
      return;

   list_for_each_entry_safe(struct npt_tls_ring, tr, &tls->tls_rings,
                            tls_head) {
      list_del(&tr->tls_head);
      npt_tls_destroy_ring(tr);
   }
   free(tls);
}

void
npt_tls_once_init(void)
{
   int state = atomic_load_explicit(&npt_tls_init_state,
                                    memory_order_acquire);
   if (state >= 2)
      return;

   int expected = 0;
   if (atomic_compare_exchange_strong_explicit(
          &npt_tls_init_state, &expected, 1,
          memory_order_acq_rel, memory_order_acquire)) {
      const int r = tss_create(&npt_tls_key, npt_tls_free);
      atomic_store_explicit(&npt_tls_init_state,
                            r == thrd_success ? 2 : 3,
                            memory_order_release);
      if (r != thrd_success)
         npt_log("npt_tls: tss_create failed, falling back to primary "
                 "ring for every thread");
   } else {
      while (atomic_load_explicit(&npt_tls_init_state,
                                  memory_order_acquire) < 2)
         thrd_yield();
   }
}

static bool
npt_tls_key_valid(void)
{
   return atomic_load_explicit(&npt_tls_init_state,
                               memory_order_acquire) == 2;
}

static struct npt_tls *
npt_tls_get(void)
{
   if (!npt_tls_key_valid())
      return NULL;

   struct npt_tls *tls = tss_get(npt_tls_key);
   if (tls)
      return tls;

   tls = calloc(1, sizeof(*tls));
   if (!tls)
      return NULL;
   list_inithead(&tls->tls_rings);
   if (tss_set(npt_tls_key, tls) != thrd_success) {
      free(tls);
      return NULL;
   }
   return tls;
}

/* This thread's TLS ring for dev, or NULL if it has none yet. */
static struct npt_ring *
npt_tls_find_ring(struct npt_tls *tls, struct npt_device *dev)
{
   list_for_each_entry(struct npt_tls_ring, tr, &tls->tls_rings,
                       tls_head) {
      struct npt_ring *ring =
         atomic_load_explicit(&tr->ring, memory_order_acquire);
      if (tr->device == dev && ring)
         return ring;
   }
   return NULL;
}

struct npt_ring *
npt_tls_peek_ring(struct npt_device *dev)
{
   if (!dev || !dev->multi_ring_enabled)
      return NULL;
   struct npt_tls *tls = npt_tls_get();
   return tls ? npt_tls_find_ring(tls, dev) : NULL;
}

struct npt_ring *
npt_tls_get_ring(struct npt_device *dev)
{
   if (!dev)
      return NULL;

   /* DC/SC wrappers carry their own instance_ring; everything else
    * funnels to primary unless multi_ring_enabled. */
   if (!dev->multi_ring_enabled)
      return dev->ring;

   struct npt_tls *tls = npt_tls_get();
   if (unlikely(!tls))
      return dev->ring;

   struct npt_ring *found = npt_tls_find_ring(tls, dev);
   if (found)
      return found;

   struct npt_tls_ring *tr = calloc(1, sizeof(*tr));
   if (!tr)
      return dev->ring;

   mtx_init(&tr->mutex, mtx_plain);
   tr->device = dev;

   struct npt_ring_layout layout;
   npt_ring_get_layout(npt_tls_ring_buffer_size(), NPT_TLS_RING_EXTRA_SIZE,
                       &layout);
   const uint64_t ring_id = atomic_fetch_add(&dev->next_ring_id, 1);

   struct npt_ring *new_ring =
      npt_ring_create(dev, &layout, ring_id, true /* is_tls_ring */);
   if (!new_ring) {
      mtx_destroy(&tr->mutex);
      free(tr);
      return dev->ring;
   }
   /* Release-store so a peer's acquire-load sees a fully-init ring. */
   atomic_store_explicit(&tr->ring, new_ring, memory_order_release);

   mtx_lock(&dev->tls_rings_mutex);
   list_addtail(&tr->dev_head, &dev->tls_rings);
   mtx_unlock(&dev->tls_rings_mutex);

   list_addtail(&tr->tls_head, &tls->tls_rings);

   /* This thread's earlier traffic went to other rings (primary before
    * the device went multi-ring, at least); the new ring's first
    * command must decode after all of it. */
   npt_ring_order_after_all(new_ring);

   return new_ring;
}

uint32_t
npt_tls_snapshot_ring_tails(struct npt_device *dev, uint64_t skip_ring_id,
                            struct npt_ring_edge *out, uint32_t cap)
{
   uint32_t n = 0;
   if (!dev)
      return 0;

#define NPT_TLS_SNAPSHOT_ADD(r)                                          \
   do {                                                                  \
      struct npt_ring *_r = (r);                                         \
      if (_r && _r->id != skip_ring_id) {                                \
         if (n < cap) {                                                  \
            out[n].ring_id = _r->id;                                     \
            out[n].seqno = npt_ring_seqno_now(_r);                       \
         }                                                               \
         n++;                                                            \
      }                                                                  \
   } while (0)

   NPT_TLS_SNAPSHOT_ADD(dev->ring);

   mtx_lock(&dev->tls_rings_mutex);
   list_for_each_entry(struct npt_tls_ring, tr, &dev->tls_rings, dev_head) {
      NPT_TLS_SNAPSHOT_ADD(
         atomic_load_explicit(&tr->ring, memory_order_acquire));
   }
   mtx_unlock(&dev->tls_rings_mutex);

   mtx_lock(&dev->instance_rings_mutex);
   list_for_each_entry(struct npt_ring, ir, &dev->instance_rings,
                       instance_head) {
      NPT_TLS_SNAPSHOT_ADD(ir);
   }
   mtx_unlock(&dev->instance_rings_mutex);
#undef NPT_TLS_SNAPSHOT_ADD

   return n;
}

bool
npt_tls_ring_tail_by_id(struct npt_device *dev, uint64_t ring_id,
                        uint32_t *out_tail)
{
   if (!dev)
      return false;
   if (dev->ring && dev->ring->id == ring_id) {
      *out_tail = npt_ring_seqno_now(dev->ring);
      return true;
   }

   bool found = false;
   mtx_lock(&dev->tls_rings_mutex);
   list_for_each_entry(struct npt_tls_ring, tr, &dev->tls_rings, dev_head) {
      struct npt_ring *ring =
         atomic_load_explicit(&tr->ring, memory_order_acquire);
      if (ring && ring->id == ring_id) {
         *out_tail = npt_ring_seqno_now(ring);
         found = true;
         break;
      }
   }
   mtx_unlock(&dev->tls_rings_mutex);
   if (found)
      return true;

   mtx_lock(&dev->instance_rings_mutex);
   list_for_each_entry(struct npt_ring, ir, &dev->instance_rings,
                       instance_head) {
      if (ir->id == ring_id) {
         *out_tail = npt_ring_seqno_now(ir);
         found = true;
         break;
      }
   }
   mtx_unlock(&dev->instance_rings_mutex);
   return found;
}

void
npt_tls_destroy_ring(struct npt_tls_ring *tr)
{
   if (!tr)
      return;

   mtx_lock(&tr->mutex);
   struct npt_ring *ring =
      atomic_load_explicit(&tr->ring, memory_order_relaxed);
   if (ring) {
      /* First owner: destroy the ring, leave tr for the second owner. */
      atomic_store_explicit(&tr->ring, (struct npt_ring *)NULL,
                            memory_order_release);
      /* Prune from dev->tls_rings to keep the drain walk tight. */
      struct npt_device *dev = tr->device;
      mtx_unlock(&tr->mutex);

      if (dev) {
         mtx_lock(&dev->tls_rings_mutex);
         if (!list_is_empty(&tr->dev_head))
            list_del(&tr->dev_head);
         list_inithead(&tr->dev_head);
         mtx_unlock(&dev->tls_rings_mutex);
      }

      npt_ring_destroy(ring);
   } else {
      /* Second owner. */
      mtx_unlock(&tr->mutex);
      mtx_destroy(&tr->mutex);
      free(tr);
   }
}
