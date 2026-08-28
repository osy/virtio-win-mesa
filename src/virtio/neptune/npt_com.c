/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_env.h"
#include "npt_overrides.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "npt_ring.h"

#include <stdatomic.h>

#if defined(__WINE__)
#include "nptunix/npt_unixlib.h"

/* Wine PE build: ids are allocated from a process-wide counter held by
 * the unixlib so d3d11.dll / dxgi.dll (which share the single host
 * context wired up by npt_device_acquire) don't collide. */
uint64_t
npt_com_allocate_next_id(void)
{
   if (npt_unixlib_ensure_init() != 0)
      return 0;
   struct npt_unix_alloc_obj_id_params p = { 0 };
   npt_wine_unix_call(npt_unix_alloc_obj_id, &p);
   return p.result_id;
}
#else
/* Monotonic guest-allocated object id. */
static _Atomic uint64_t g_npt_next_obj_id = 1;

uint64_t
npt_com_allocate_next_id(void)
{
   return atomic_fetch_add_explicit(&g_npt_next_obj_id, 1,
                                    memory_order_relaxed);
}
#endif

#include "neptune-protocol/npt_protocol_client_table.h"
#include "neptune-protocol/npt_protocol_defs.h"

#include <stdlib.h>
#include <string.h>

/*
 * IID -> ctor table.  Open-addressing hash, 256 buckets for the ~190
 * interfaces plus headroom.  Populated at init; lookups are lock-free.
 */
#define NPT_COM_CTOR_TABLE_BUCKETS 256u

struct npt_com_ctor_entry {
   const GUID *iid;        /* NULL marks an empty bucket */
   npt_com_ctor_fn ctor;
   size_t aux_size;
   npt_com_aux_init_fn aux_init;
};

static struct npt_com_ctor_entry g_ctor_table[NPT_COM_CTOR_TABLE_BUCKETS];

static inline uint32_t
npt_com_iid_hash(const GUID *iid)
{
   const uint32_t *w = (const uint32_t *)iid;
   uint32_t h = w[0] ^ w[1] ^ w[2] ^ w[3];
   return (uint32_t)((h * 0x9E3779B1u) & (NPT_COM_CTOR_TABLE_BUCKETS - 1u));
}

static inline bool
npt_com_iid_equal(const GUID *a, const GUID *b)
{
   return memcmp(a, b, sizeof(GUID)) == 0;
}

/* Returns NULL only when the table is full. */
static struct npt_com_ctor_entry *
npt_com_table_lookup_entry_mut(const GUID *iid)
{
   if (!iid)
      return NULL;
   uint32_t h = npt_com_iid_hash(iid);
   for (uint32_t i = 0; i < NPT_COM_CTOR_TABLE_BUCKETS; i++) {
      uint32_t idx = (h + i) & (NPT_COM_CTOR_TABLE_BUCKETS - 1u);
      struct npt_com_ctor_entry *e = &g_ctor_table[idx];
      if (e->iid == NULL)
         return e;
      if (npt_com_iid_equal(e->iid, iid))
         return e;
   }
   return NULL;
}

/* == QueryInterface verdict memo ====================================
 * A host-backed QueryInterface is a synchronous ring round trip, and
 * COM identity rules fix an object's answer for a given IID for its
 * whole lifetime -- so both the verdict and the resulting wrapper are
 * memoized on the source wrapper.  Repeat QIs for the same (object,
 * IID) return the same interface pointer with a fresh public ref and
 * no wire traffic.
 *
 * Each positive entry owns one public ref on the memoized wrapper.
 * The memo flushes when the source's public count drains (the caller
 * of a QI must itself hold a public ref, so no lookup can race the
 * flush) and again from npt_com_destroy for the force-destroy path;
 * there the release is gated on the wrapper cache still knowing the
 * wrapper, because the device-teardown drain frees wrappers in
 * arbitrary order. */

struct npt_com_qi_memo_entry {
   struct npt_com_qi_memo_entry *next;
   GUID iid;
   struct npt_com_base *wrapper; /* NULL = host answered E_NOINTERFACE */
};

static mtx_t g_qi_memo_mutex;

typedef uint32_t (NPT_STDMETHODCALLTYPE *npt_com_unknown_ref_fn)(void *);

static uint32_t
npt_com_vtbl_addref(struct npt_com_base *com)
{
   return ((npt_com_unknown_ref_fn)com->lpVtbl[1])(com);
}

static uint32_t
npt_com_vtbl_release(struct npt_com_base *com)
{
   return ((npt_com_unknown_ref_fn)com->lpVtbl[2])(com);
}

static struct npt_com_qi_memo_entry *
npt_com_qi_memo_find_locked(struct npt_com_base *com, const GUID *riid)
{
   for (struct npt_com_qi_memo_entry *e = com->qi_memo; e; e = e->next)
      if (npt_com_iid_equal(&e->iid, riid))
         return e;
   return NULL;
}

/* Idempotent; releases outside the lock so a memoized wrapper's own
 * teardown (which flushes its memo in turn) cannot self-deadlock. */
static void
npt_com_qi_memo_flush(struct npt_com_base *com)
{
   mtx_lock(&g_qi_memo_mutex);
   struct npt_com_qi_memo_entry *e = com->qi_memo;
   com->qi_memo = NULL;
   mtx_unlock(&g_qi_memo_mutex);
   while (e) {
      struct npt_com_qi_memo_entry *next = e->next;
      if (e->wrapper &&
          npt_device_wrapper_cache_is_live(com->base.device,
                                           e->wrapper->base.id, e->wrapper))
         npt_com_vtbl_release(e->wrapper);
      free(e);
      e = next;
   }
}

/* Override families patch vtbls in place rather than registering a
 * different ctor, so existing entries are left alone. */
void
npt_com_register_default_ctor(const GUID *iid, npt_com_ctor_fn ctor)
{
   if (!iid || !ctor)
      return;
   struct npt_com_ctor_entry *e = npt_com_table_lookup_entry_mut(iid);
   if (!e) {
      npt_log("npt_com: ctor table full, dropping registration");
      return;
   }
   if (e->iid == NULL) {
      e->iid = iid;
      e->ctor = ctor;
   }
}


void
npt_com_register_family(const GUID *const *tier_iids,
                        size_t aux_size,
                        npt_com_aux_init_fn aux_init)
{
   if (!tier_iids)
      return;
   for (const GUID *const *p = tier_iids; *p; p++) {
      struct npt_com_ctor_entry *e = npt_com_table_lookup_entry_mut(*p);
      /* Empty slot means register_family ran before the default-ctor
       * populator -- aux metadata would attach to no specific IID. */
      if (!e || e->iid == NULL) {
         npt_log("npt_com_register_family: no ctor entry for IID "
                 "(register_family must run after default-ctor populator)");
         continue;
      }
      e->aux_size = aux_size;
      e->aux_init = aux_init;
   }
}

uint32_t NPT_STDMETHODCALLTYPE
npt_com_default_addref(void *self)
{
   struct npt_com_base *com = self;
   uint32_t prev = atomic_fetch_add(&com->base.pub_ref, 1);
   if (prev == 0) {
      /* 0->1: take a priv hold on self and parent. */
      atomic_fetch_add(&com->base.priv_ref, 1);
      if (com->base.parent)
         atomic_fetch_add(&com->base.parent->base.priv_ref, 1);
   }
   return prev + 1;
}

static void npt_com_destroy(struct npt_com_base *com);
static struct npt_ring *npt_com_acquire_dc_sc_ring(struct npt_device *dev);
static void npt_com_release_dc_sc_ring(struct npt_device *dev);

uint32_t NPT_STDMETHODCALLTYPE
npt_com_default_release(void *self)
{
   struct npt_com_base *com = self;
   uint32_t prev = atomic_fetch_sub(&com->base.pub_ref, 1);
   const uint32_t cur = prev - 1;
   if (cur == 0) {
      /* Drop the QI memo's wrapper holds while our own 0->1 priv hold
       * is still in place: each memoized wrapper's release decrements
       * our priv_ref (parent coupling), and that hold keeps the count
       * above zero so the cascade cannot destroy us mid-flush. */
      npt_com_qi_memo_flush(com);
      /* 1->0: drop the matching priv holds taken on 0->1. */
      struct npt_com_base *parent = com->base.parent;
      uint32_t priv = atomic_fetch_sub(&com->base.priv_ref, 1) - 1;
      if (priv == 0)
         npt_com_destroy(com);
      if (parent) {
         uint32_t parent_priv =
            atomic_fetch_sub(&parent->base.priv_ref, 1) - 1;
         if (parent_priv == 0 && atomic_load(&parent->base.pub_ref) == 0)
            npt_com_destroy(parent);
      }
   }
   return cur;
}

void
npt_com_force_destroy(struct npt_com_base *com)
{
   if (com)
      npt_com_destroy(com);
}

void *
npt_com_get_or_wrap_or_release(struct npt_device *dev, const GUID *iid,
                               uint64_t host_id,
                               struct npt_com_base *parent_wrapper)
{
   if (!dev || !host_id)
      return NULL;
   void *wrapper = npt_com_get_or_wrap(dev, iid, host_id, parent_wrapper);
   if (!wrapper) {
      /* Host added a ref before returning host_id; drain it now. */
      npt_com_send_release(dev, host_id);
   }
   return wrapper;
}

static void
npt_com_destroy(struct npt_com_base *com)
{
   /* Re-entry guard: aux_destroy may release cached child wrappers
    * (swapchain backbuffer cache, device immediate-context cache).  A
    * child's default_release decrements its parent's priv_ref via the
    * 0->1 AddRef matching pattern, and when the last cached child
    * drops the parent's priv_ref to 0 with pub_ref already 0,
    * default_release would recurse into npt_com_destroy on the same
    * parent -- freeing aux/com while the outer call is still running.
    * Bumping priv_ref here keeps every in-aux_destroy decrement >=1,
    * so the cascade gate inside default_release stays shut. */
   atomic_fetch_add_explicit(&com->base.priv_ref, 1, memory_order_relaxed);

   /* Backstop for force-destroy, which bypasses default_release; a
    * release-driven destroy already flushed and this is a no-op. */
   npt_com_qi_memo_flush(com);

   /* Order: cache_remove (concurrent lookup misses us), COM_RELEASE,
    * DC/SC ring unref (after ring's batch sees the COM_RELEASE),
    * aux_destroy, free, then drop device refs (the last release may
    * destroy dev). */
   if (com->base.device)
      npt_device_wrapper_cache_remove(com->base.device, com->base.id);
   npt_com_send_release(com->base.device, com->base.id);
   /* Pinned to primary: instance_ring == dev->ring, no refcount taken. */
   if (com->base.device && com->base.instance_ring) {
      if (com->base.private_instance_ring) {
         struct npt_device *dev = com->base.device;
         mtx_lock(&dev->instance_rings_mutex);
         list_del(&com->base.instance_ring->instance_head);
         mtx_unlock(&dev->instance_rings_mutex);
         npt_ring_destroy(com->base.instance_ring);
      } else if (com->base.instance_ring != com->base.device->ring) {
         npt_com_release_dc_sc_ring(com->base.device);
      }
      com->base.instance_ring = NULL;
      com->base.private_instance_ring = false;
   }
   if (com->aux_destroy)
      com->aux_destroy(com->aux);
   atomic_fetch_sub_explicit(&com->base.priv_ref, 1, memory_order_relaxed);
   const uint32_t device_refs =
      atomic_load_explicit(&com->base.device_ref_holds, memory_order_relaxed);
   free(com);
   for (uint32_t i = 0; i < device_refs; i++)
      npt_device_release();
}

/*
 * Pin DC* / SC* IIDs (single-threaded-per-instance by D3D11/DXGI
 * contract) onto the shared DC/SC ring so host dispatch stays serial.
 * Off when multi_ring_enabled is false (primary is then the only
 * dispatch thread, so there's nothing to protect).
 */
static bool
npt_com_iid_needs_instance_ring(const struct npt_device *dev,
                                const GUID *iid)
{
   if (!dev->multi_ring_enabled)
      return false;
   static const GUID *const dc_sc_iids[] = {
      &NPT_IID_ID3D11DeviceContext,
      &NPT_IID_ID3D11DeviceContext1,
      &NPT_IID_ID3D11DeviceContext2,
      &NPT_IID_ID3D11DeviceContext3,
      &NPT_IID_ID3D11DeviceContext4,
      &NPT_IID_IDXGISwapChain,
      &NPT_IID_IDXGISwapChain1,
      &NPT_IID_IDXGISwapChain2,
      &NPT_IID_IDXGISwapChain3,
      &NPT_IID_IDXGISwapChain4,
      &NPT_IID_IDXGISwapChainMedia,
      /* D3D12 command queues are pinned explicitly by type via
       * npt_com_pin_queue_ring (the generic path can't see the queue
       * desc); command lists deliberately stay on TLS rings for
       * parallel recording. */
   };
   for (size_t i = 0;
        i < sizeof(dc_sc_iids) / sizeof(dc_sc_iids[0]); i++) {
      if (npt_com_iid_equal(iid, dc_sc_iids[i]))
         return true;
   }
   return false;
}

/*
 * Lazily create dev's shared DC/SwapChain ring.  DXGI requires Present
 * on the same thread as the immediate context, and the host D3D
 * library has no internal locks coordinating DC vs SC vs Device, so
 * every DC* and SC* wrapper multiplexes through one ring per device.
 */
static struct npt_ring *
npt_com_acquire_dc_sc_ring(struct npt_device *dev)
{
   struct npt_ring *ring =
      atomic_load_explicit(&dev->dc_sc_ring, memory_order_acquire);
   if (!ring) {
      mtx_lock(&dev->instance_rings_mutex);
      ring = atomic_load_explicit(&dev->dc_sc_ring, memory_order_relaxed);
      if (!ring) {
         const size_t extra_size = sizeof(uint32_t);
         struct npt_ring_layout layout;
         npt_ring_get_layout(NPT_DC_SC_RING_BUFFER_SIZE, extra_size, &layout);
         const uint64_t ring_id = atomic_fetch_add(&dev->next_ring_id, 1);
         ring = npt_ring_create(dev, &layout, ring_id,
                                false /* is_tls_ring */);
         if (ring) {
            list_addtail(&ring->instance_head, &dev->instance_rings);
            atomic_store_explicit(&dev->dc_sc_ring, ring,
                                  memory_order_release);
         }
      }
      mtx_unlock(&dev->instance_rings_mutex);
   }
   if (ring)
      atomic_fetch_add_explicit(&dev->dc_sc_ring_refs, 1,
                                memory_order_relaxed);
   return ring;
}

void *
npt_com_family_aux(void *self, void (*aux_destroy)(void *aux))
{
   struct npt_com_base *com = self;
   if (!com || !com->aux)
      return NULL;
   if (com->aux_destroy == aux_destroy)
      return com->aux;
   /* Tier alias: aux_destroy was cleared and aux points at a same-
    * family ancestor's aux (npt_com_get_or_wrap).  Confirm the shared
    * aux really belongs to the requested family via that ancestor
    * before handing it back, so a foreign-family alias never aliases in
    * as ours. */
   if (com->aux_destroy == NULL) {
      for (struct npt_com_base *anc = com->base.parent; anc;
           anc = anc->base.parent) {
         if (!anc->aux_destroy)
            continue;
         return (anc->aux_destroy == aux_destroy && anc->aux == com->aux)
                   ? com->aux : NULL;
      }
   }
   return NULL;
}

static void npt_com_release_dc_sc_ring(struct npt_device *dev);

struct npt_ring *
npt_com_object_ring_seqno(void *self, uint32_t *out_seqno)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   if (out_seqno)
      *out_seqno = ring ? npt_ring_seqno_now(ring) : 0;
   return ring;
}

void
npt_com_pin_queue_ring(void *self, bool direct)
{
   struct npt_com_base *com = self;
   struct npt_device *dev = com ? com->base.device : NULL;
   if (!dev || !dev->multi_ring_enabled || com->base.instance_ring)
      return;

   if (direct) {
      /* Share the DC/SC ring so submissions serialize with present. */
      com->base.instance_ring = npt_com_acquire_dc_sc_ring(dev);
      return;
   }

   struct npt_ring_layout layout;
   npt_ring_get_layout(NPT_DC_SC_RING_BUFFER_SIZE, sizeof(uint32_t),
                       &layout);
   const uint64_t ring_id = atomic_fetch_add(&dev->next_ring_id, 1);
   struct npt_ring *ring = npt_ring_create(dev, &layout, ring_id,
                                           false /* is_tls_ring */);
   if (!ring)
      return; /* fall back to the caller's TLS ring */

   mtx_lock(&dev->instance_rings_mutex);
   list_addtail(&ring->instance_head, &dev->instance_rings);
   mtx_unlock(&dev->instance_rings_mutex);

   com->base.instance_ring = ring;
   com->base.private_instance_ring = true;
}

static void
npt_com_release_dc_sc_ring(struct npt_device *dev)
{
   if (atomic_fetch_sub_explicit(&dev->dc_sc_ring_refs, 1,
                                 memory_order_acq_rel) != 1)
      return;
   mtx_lock(&dev->instance_rings_mutex);
   struct npt_ring *ring =
      atomic_load_explicit(&dev->dc_sc_ring, memory_order_relaxed);
   /* Re-check refs: another thread may have acquired between our
    * dec and the mutex. */
   if (ring &&
       atomic_load_explicit(&dev->dc_sc_ring_refs, memory_order_relaxed) == 0) {
      list_del(&ring->instance_head);
      atomic_store_explicit(&dev->dc_sc_ring, (struct npt_ring *)NULL,
                            memory_order_release);
      mtx_unlock(&dev->instance_rings_mutex);
      npt_ring_destroy(ring);
      return;
   }
   mtx_unlock(&dev->instance_rings_mutex);
}

void *
npt_com_get_or_wrap(struct npt_device *dev, const GUID *iid,
                    uint64_t host_id, struct npt_com_base *parent_wrapper)
{
   if (!dev || !host_id)
      return NULL;

   /*
    * Fast path: cache hit bumps pub_ref via CAS.  Slow path: miss,
    * construct, publish atomically.  Races:
    *  - two ctors race: insert_if_absent returns the loser the winner
    *    so we discard our own.
    *  - cache slot occupied by a dying wrapper (pub==0, mid-destroy):
    *    insert_if_absent flags retry_needed; we yield and re-lookup
    *    so the dying wrapper's cache_remove + COM_RELEASE stay paired.
    */
   for (uint32_t tries = 0; tries < 8; tries++) {
      void *cached = npt_device_wrapper_cache_lookup(dev, host_id);
      if (cached)
         return cached;

      const struct npt_com_ctor_entry *entry =
         npt_com_table_lookup_entry_mut(iid);
      if (!entry || !entry->ctor) {
         npt_log("npt_com_get_or_wrap: no ctor registered for IID");
         return NULL;
      }

      void *wrapper = entry->ctor(dev, host_id);
      if (!wrapper)
         return NULL;

      struct npt_com_base *com = wrapper;

      /* Set parent BEFORE aux_init so per-family inits can detect a
       * tier alias (parent in the same family) and share the parent's
       * aux instead of standing up duplicate external resources. */
      com->base.parent = parent_wrapper;
      if (parent_wrapper)
         atomic_fetch_add(&parent_wrapper->base.priv_ref, 1);
      /* Mirrors default_addref: priv_ref tracks the live pub_ref hold. */
      atomic_fetch_add(&com->base.priv_ref, 1);

      /* On aux OOM the wrapper is still usable; aux-dependent
       * accessors see NULL. */
      if (entry->aux_size) {
         com->aux = calloc(1, entry->aux_size);
         if (com->aux && entry->aux_init)
            entry->aux_init(com, dev, host_id);
         /* Tier alias: if a same-family ancestor already owns an aux,
          * tear down our freshly-init aux and point at the ancestor's
          * so per-tier wrappers share one set of external resources.
          * The walk skips NULL-destroy intermediates so an
          * alias-of-an-alias still finds the primary; a different-
          * family ancestor has a non-matching destroy and stops the
          * walk.  Aux destructors must be reentrant on a freshly-init
          * aux. */
         if (com->aux_destroy) {
            for (struct npt_com_base *anc = parent_wrapper; anc;
                 anc = anc->base.parent) {
               if (!anc->aux_destroy)
                  continue;
               if (anc->aux_destroy == com->aux_destroy && anc->aux) {
                  com->aux_destroy(com->aux);
                  com->aux = anc->aux;
                  com->aux_destroy = NULL;
               }
               break;
            }
         }
      }

      if (!com->base.instance_ring &&
          npt_com_iid_needs_instance_ring(dev, iid)) {
         com->base.instance_ring = npt_com_acquire_dc_sc_ring(dev);
      }

      bool retry = false;
      void *winner =
         npt_device_wrapper_cache_insert_if_absent(dev, host_id,
                                                   wrapper, &retry);
      if (winner) {
         /* Lost the race.  Roll back speculative holds and free our
          * wrapper directly -- it was never in the cache, and the
          * host ref is owned by the winner. */
         if (parent_wrapper)
            atomic_fetch_sub(&parent_wrapper->base.priv_ref, 1);
         if (com->base.instance_ring) {
            if (com->base.instance_ring != dev->ring)
               npt_com_release_dc_sc_ring(dev);
            com->base.instance_ring = NULL;
         }
         atomic_store(&com->base.pub_ref, 0);
         atomic_store(&com->base.priv_ref, 0);
         if (com->aux_destroy)
            com->aux_destroy(com->aux);
         free(com);
         return winner;
      }

      if (!retry)
         return wrapper;

      /* Slot occupied by a dying wrapper.  Yield and retry. */
      if (parent_wrapper)
         atomic_fetch_sub(&parent_wrapper->base.priv_ref, 1);
      if (com->base.instance_ring) {
         if (com->base.instance_ring != dev->ring)
            npt_com_release_dc_sc_ring(dev);
         com->base.instance_ring = NULL;
      }
      atomic_store(&com->base.pub_ref, 0);
      atomic_store(&com->base.priv_ref, 0);
      if (com->aux_destroy)
         com->aux_destroy(com->aux);
      free(com);
      thrd_yield();
   }

   npt_log("npt_com_get_or_wrap: gave up after 8 retries on host_id contention");
   return NULL;
}

HRESULT
npt_com_default_query_interface_chain(void *self, const GUID *riid,
                                      void **out, const GUID *const *chain)
{
   if (!out)
      return NPT_E_POINTER;
   *out = NULL;
   if (!riid || !self)
      return NPT_E_POINTER;
   for (const GUID *const *p = chain; *p; p++) {
      if (npt_com_iid_equal(*p, riid)) {
         npt_com_default_addref(self);
         *out = self;
         return NPT_S_OK;
      }
   }
   return NPT_E_NOINTERFACE;
}

void
npt_com_send_release(struct npt_device *dev, uint64_t host_id)
{
   if (npt_com_id_is_guest_fab(host_id))
      return;
   npt_ring_send_com_release(dev, host_id);
}

static HRESULT
npt_com_send_query_interface(struct npt_device *dev, uint64_t src_id,
                             const GUID *iid, uint64_t guest_id)
{
   if (!dev || !dev->ring)
      return NPT_E_FAIL;
   return npt_dispatch_com_query_interface(dev->ring, src_id, iid, guest_id);
}

HRESULT
npt_com_query_interface_host(void *self, const GUID *riid, void **ppvObject)
{
   struct npt_com_base *com = self;
   if (!ppvObject)
      return NPT_E_POINTER;
   *ppvObject = NULL;
   if (!riid || !self)
      return NPT_E_POINTER;

   mtx_lock(&g_qi_memo_mutex);
   struct npt_com_qi_memo_entry *e = npt_com_qi_memo_find_locked(com, riid);
   if (e) {
      struct npt_com_base *w = e->wrapper;
      mtx_unlock(&g_qi_memo_mutex);
      if (!w)
         return NPT_E_NOINTERFACE;
      /* The caller holds a public ref on `com`, so the memo cannot
       * flush (and w cannot die) between the unlock and this AddRef. */
      npt_com_vtbl_addref(w);
      *ppvObject = w;
      return NPT_S_OK;
   }
   mtx_unlock(&g_qi_memo_mutex);

   /* One host round trip per (object, IID).  The guest id is
    * pre-allocated and the wrapper built speculatively; on
    * E_NOINTERFACE the id was never registered host-side and the
    * wrapper's COM_RELEASE is a no-op there.  parent=self so the
    * wrapper inherits the instance ring and shares family aux with
    * the object it aliases. */
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t guest_id = npt_com_allocate_next_id();
   struct npt_com_base *wrapper =
      npt_com_get_or_wrap(dev, riid, guest_id, com);
   if (!wrapper)
      return NPT_E_OUTOFMEMORY;
   HRESULT hr = npt_com_send_query_interface(dev, npt_com_self_id(self),
                                             riid, guest_id);
   if (hr != NPT_S_OK && hr != NPT_E_NOINTERFACE) {
      /* Transport failure: nothing was learned about the IID. */
      npt_com_vtbl_release(wrapper);
      return hr;
   }

   struct npt_com_qi_memo_entry *fresh = malloc(sizeof(*fresh));
   struct npt_com_base *keep = NULL;
   bool discard = false;
   mtx_lock(&g_qi_memo_mutex);
   struct npt_com_qi_memo_entry *prior =
      npt_com_qi_memo_find_locked(com, riid);
   if (prior) {
      /* A concurrent probe won; its entry is authoritative. */
      if (prior->wrapper) {
         npt_com_vtbl_addref(prior->wrapper);
         keep = prior->wrapper;
      }
      discard = true;
   } else if (fresh) {
      fresh->iid = *riid;
      if (hr == NPT_S_OK) {
         fresh->wrapper = wrapper;
         npt_com_vtbl_addref(wrapper); /* the memo's hold */
         keep = wrapper;
      } else {
         fresh->wrapper = NULL;
         discard = true;
      }
      fresh->next = com->qi_memo;
      com->qi_memo = fresh;
      fresh = NULL;
   } else {
      /* Entry allocation failed: answer unmemoized. */
      if (hr == NPT_S_OK)
         keep = wrapper;
      else
         discard = true;
   }
   mtx_unlock(&g_qi_memo_mutex);
   free(fresh);
   if (discard)
      npt_com_vtbl_release(wrapper);
   if (!keep)
      return NPT_E_NOINTERFACE;
   *ppvObject = keep;
   return NPT_S_OK;
}

void
npt_com_assert_overridden(const char *iid_name, const char *method_name)
{
   npt_log("npt_com: %s::%s called without override -- this method requires "
           "a hand-written wrapper because its parameters cannot be "
           "auto-marshalled (skip_default).  Aborting.",
           iid_name ? iid_name : "?",
           method_name ? method_name : "?");
   abort();
}

/*
 * Explicit one-time init (not __attribute__((constructor))) to avoid
 * the Wine PE loader-lock deadlock: a ctor that took the loader lock
 * or synchronously hit a Unix bridge would deadlock against Wine's
 * PE loader.
 */

static _Atomic int g_npt_com_init_state;

static void
npt_com_init_impl(void)
{
   /* env_init first so subsequent NPT_PERF(...) branches see the bitmask. */
   npt_env_init();
   npt_profile_init();
   mtx_init(&g_qi_memo_mutex, mtx_plain);

   /* Default ctors must run before override installers below read the
    * populated default vtbls. */
   npt_com_init_default_ctors();

   npt_overrides_d3d11_device_init();
   npt_overrides_d3d11_context_init();
   npt_overrides_d3d11_buffer_init();
   npt_overrides_d3d11_texture_init();
   npt_overrides_d3d11_view_init();
   npt_overrides_dxgi_output_init();
   npt_overrides_dxgi_factory_init();
   npt_overrides_dxgi_factorymedia_init();
   npt_overrides_dxgi_adapter_init();
   npt_overrides_d3d11_fence_init();
   npt_overrides_d3d11_query_init();

   npt_overrides_d3d12_device_init();
   npt_overrides_d3d12_resource_init();
   npt_overrides_d3d12_fence_init();
   npt_overrides_d3d12_queue_init();
   npt_overrides_d3d12_list_init();
   npt_d3d12_heap_overrides_init();
}

void
npt_com_init(void)
{
   NPT_CALL_ONCE(g_npt_com_init_state, npt_com_init_impl());
}
