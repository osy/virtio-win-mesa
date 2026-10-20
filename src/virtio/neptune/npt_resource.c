/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_renderer.h"
#include "npt_resource.h"
#include "npt_ring.h"
#include "npt_shmem_pool.h"

#include "util/list.h"
#include "util/u_atomic.h"

#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ==========================================================================
 * Map shadow-staging rename ring
 * ========================================================================== */

/* Async shmem destroy worker.  Each slot teardown is munmap + virtgpu
 * gem_close (two Wine syscalls); funneling them through a singleton
 * thread keeps the Release path from blocking on kernel ioctl
 * turnaround.  Correctness: COM_RELEASE was queued in npt_com_destroy
 * before aux_destroy, and the host blob view is independent of the
 * guest mmap, so destroying out of order is safe.  Lazy-init, detached
 * worker; process exit doesn't drain — the kernel reclaims leftover
 * GEM handles + munmaps.
 *
 * The worker lives as long as the process, so the module it runs in must
 * too.  The D3D runtime unloads a UMD when its last adapter closes, and
 * D3D9On12 does so between devices; the next load at the same base
 * re-initialises g_shmem_reaper underneath the previous load's worker,
 * which then waits on a lock that is no longer one and faults inside
 * EnterCriticalSection.  So on Windows the module is pinned when the
 * worker starts. */
struct shmem_reap_entry {
   struct npt_renderer *renderer;
   struct npt_renderer_shmem *shmem;
   struct list_head head;
};

static struct {
   _Atomic int state;            /* see NPT_CALL_ONCE: 0=uninit, 1=in-progress, 2=ready */
   mtx_t mutex;
   cnd_t cond;
   struct list_head queue;
   /* A detached batch is being destroyed outside the lock; idle_cond is
    * broadcast when it lands.  Drains wait on it, since the batch may
    * name the renderer being torn down. */
   bool busy;
   cnd_t idle_cond;
} g_shmem_reaper;

#if defined(_WIN32)
static DWORD WINAPI shmem_reaper_main(LPVOID arg)
#else
static void *shmem_reaper_main(void *arg)
#endif
{
   (void)arg;
   for (;;) {
      mtx_lock(&g_shmem_reaper.mutex);
      while (list_is_empty(&g_shmem_reaper.queue))
         cnd_wait(&g_shmem_reaper.cond, &g_shmem_reaper.mutex);
      /* Detach the whole queue so the destroys run outside the lock. */
      struct list_head batch;
      list_replace(&g_shmem_reaper.queue, &batch);
      list_inithead(&g_shmem_reaper.queue);
      g_shmem_reaper.busy = true;
      mtx_unlock(&g_shmem_reaper.mutex);

      while (!list_is_empty(&batch)) {
         struct shmem_reap_entry *e =
            list_first_entry(&batch, struct shmem_reap_entry, head);
         list_del(&e->head);
         e->renderer->shmem_ops.destroy(e->renderer, e->shmem);
         free(e);
      }

      mtx_lock(&g_shmem_reaper.mutex);
      g_shmem_reaper.busy = false;
      cnd_broadcast(&g_shmem_reaper.idle_cond);
      mtx_unlock(&g_shmem_reaper.mutex);
   }
#if defined(_WIN32)
   return 0;
#else
   return NULL;
#endif
}

static void
shmem_reaper_init_impl(void)
{
   mtx_init(&g_shmem_reaper.mutex, mtx_plain);
   cnd_init(&g_shmem_reaper.cond);
   cnd_init(&g_shmem_reaper.idle_cond);
   g_shmem_reaper.busy = false;
   list_inithead(&g_shmem_reaper.queue);
#if defined(_WIN32)
   HMODULE self;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_PIN,
                      (LPCWSTR)(void *)shmem_reaper_main, &self);
   HANDLE h = CreateThread(NULL, 0, shmem_reaper_main, NULL, 0, NULL);
   if (h) CloseHandle(h);
#else
   pthread_t t;
   if (pthread_create(&t, NULL, shmem_reaper_main, NULL) == 0)
      pthread_detach(t);
#endif
}

static void
shmem_reaper_lazy_init(void)
{
   NPT_CALL_ONCE(g_shmem_reaper.state, shmem_reaper_init_impl());
}

static void
shmem_reaper_unref(struct npt_renderer *renderer,
                   struct npt_renderer_shmem *shmem)
{
   if (!shmem || !npt_refcount_dec(&shmem->refcount))
      return;

   shmem_reaper_lazy_init();

   struct shmem_reap_entry *e = malloc(sizeof(*e));
   if (!e) {
      /* Allocation failure: synchronous fallback (leaks would compound). */
      renderer->shmem_ops.destroy(renderer, shmem);
      return;
   }
   e->renderer = renderer;
   e->shmem    = shmem;

   mtx_lock(&g_shmem_reaper.mutex);
   list_addtail(&e->head, &g_shmem_reaper.queue);
   cnd_signal(&g_shmem_reaper.cond);
   mtx_unlock(&g_shmem_reaper.mutex);
}

void
npt_renderer_shmem_unref_async(struct npt_renderer *renderer,
                               struct npt_renderer_shmem *shmem)
{
   shmem_reaper_unref(renderer, shmem);
}

void
npt_renderer_shmem_reaper_drain(struct npt_renderer *renderer)
{
   /* Nothing was ever queued: the reaper does not exist. */
   if (atomic_load(&g_shmem_reaper.state) != 2)
      return;

   struct list_head mine;
   list_inithead(&mine);

   mtx_lock(&g_shmem_reaper.mutex);
   list_for_each_entry_safe(struct shmem_reap_entry, e,
                            &g_shmem_reaper.queue, head) {
      if (e->renderer == renderer) {
         list_del(&e->head);
         list_addtail(&e->head, &mine);
      }
   }
   /* A batch already detached may still hold this renderer's entries;
    * it finishes with the renderer alive. */
   while (g_shmem_reaper.busy)
      cnd_wait(&g_shmem_reaper.idle_cond, &g_shmem_reaper.mutex);
   mtx_unlock(&g_shmem_reaper.mutex);

   while (!list_is_empty(&mine)) {
      struct shmem_reap_entry *e =
         list_first_entry(&mine, struct shmem_reap_entry, head);
      list_del(&e->head);
      e->renderer->shmem_ops.destroy(e->renderer, e->shmem);
      free(e);
   }
}

void
npt_d3d_map_ring_init(struct npt_d3d_map_ring *r, struct npt_com_base *com)
{
   memset(r, 0, sizeof(*r));
   r->com = com;
   mtx_init(&r->lock, mtx_plain);
}

/* Caller holds r->lock. */
static bool
alloc_slot_locked(struct npt_d3d_map_ring *r, uint32_t idx)
{
   if (!r->slots || idx >= r->max_slots)
      return false;
   if (r->slots[idx].shmem)
      return true;

   struct npt_device *dev = r->com->base.device;
   struct npt_renderer *rndr = dev->renderer;
   /* Sub-allocate from the per-device map pool.  npt_shmem_pool_alloc
    * grows the pool's shmem when there isn't room; old shmem stays
    * alive via prior slot refs. */
   size_t off = 0;
   struct npt_renderer_shmem *s =
      npt_shmem_pool_alloc(rndr, &dev->map_pool, r->aligned_slot_size, &off);
   if (!s) {
      npt_log("d3d_map_ring: pool_alloc slot %u (%u bytes) failed",
              idx, r->aligned_slot_size);
      return false;
   }
   r->slots[idx].shmem = s;
   r->slots[idx].shmem_res_id = s->res_id;
   r->slots[idx].offset = (uint32_t)off;
   r->slots[idx].pending_seqno = 0;
   r->slots[idx].pending_ring = NULL;
   r->slots[idx].in_flight = false;
   if (idx + 1u > r->active_count)
      r->active_count = idx + 1u;
   return true;
}

static bool
alloc_shmem_locked(struct npt_d3d_map_ring *r, uint32_t aligned_slot_size)
{
   if (r->aligned_slot_size && r->aligned_slot_size != aligned_slot_size) {
      npt_log("d3d_map_ring: size drift %u->%u (unsupported)",
              r->aligned_slot_size, aligned_slot_size);
      return false;
   }
   if (!r->slots) {
      /* How far a DISCARD-heavy resource may run ahead of the host before
       * a rotate blocks on the slot the host has not consumed yet.  A
       * title that maps small constant buffers a few hundred times a
       * frame otherwise runs its render thread in lockstep with the host
       * ring thread while neither is saturated.  Small slots cost little
       * pool space, so they go deeper; large ones keep their pool
       * footprint. */
      const uint32_t max_slots =
         aligned_slot_size <= 4096u  ? NPT_D3D_MAP_SLOT_MAX :
         aligned_slot_size <= 65536u ? 16u : 8u;
      struct npt_d3d_map_slot *slots = calloc(max_slots, sizeof(*slots));
      if (!slots)
         return false;
      r->slots = slots;
      r->max_slots = max_slots;
      r->aligned_slot_size = aligned_slot_size;
   }
   for (uint32_t i = 0; i < NPT_D3D_MAP_SLOT_INIT; i++) {
      if (!alloc_slot_locked(r, i))
         return false;
   }
   atomic_store_explicit(&r->ready, true, memory_order_release);
   return true;
}

bool
npt_d3d_map_ring_alloc_shmem(struct npt_d3d_map_ring *r,
                             uint32_t aligned_slot_size)
{
   if (!aligned_slot_size)
      return false;
   if (atomic_load_explicit(&r->ready, memory_order_acquire)) {
      if (r->aligned_slot_size == aligned_slot_size)
         return true;
      npt_log("d3d_map_ring: size drift %u->%u (unsupported)",
              r->aligned_slot_size, aligned_slot_size);
      return false;
   }
   mtx_lock(&r->lock);
   const bool ok = alloc_shmem_locked(r, aligned_slot_size);
   mtx_unlock(&r->lock);
   return ok;
}

void
npt_d3d_map_ring_fini(struct npt_d3d_map_ring *r)
{
   if (!r->slots) {
      free(r->records);
      mtx_destroy(&r->lock);
      return;
   }
   struct npt_renderer *rndr = r->com->base.device->renderer;

   /* No pending_seqno wait: a Map's slot choice is the only consumer and
    * the ring is going away.  Host-side ordering is covered by COM_RELEASE
    * (queued in npt_com_destroy before this fini runs). */
   for (uint32_t i = 0; i < r->active_count; i++) {
      if (r->slots[i].shmem)
         shmem_reaper_unref(rndr, r->slots[i].shmem);
   }
   free(r->slots);
   r->slots = NULL;
   free(r->records);
   r->records = NULL;
   r->record_count = r->record_cap = 0;
   r->active_count = 0;
   r->aligned_slot_size = 0;
   r->max_slots = 0;
   r->current_slot = 0;
   atomic_store_explicit(&r->ready, false, memory_order_relaxed);
   mtx_destroy(&r->lock);
}

/* Caller holds r->lock, which is dropped and retaken around the wait. */
static void
wait_slot_locked(struct npt_d3d_map_ring *r, uint32_t slot)
{
   struct npt_d3d_map_slot *s = &r->slots[slot];
   if (!s->in_flight)
      return;
   struct npt_ring *const pending_ring = s->pending_ring;
   const uint32_t seqno = s->pending_seqno;
   mtx_unlock(&r->lock);
   npt_ring_wait_seqno(pending_ring ? pending_ring
                                    : r->com->base.device->ring, seqno);
   mtx_lock(&r->lock);
   /* Another Unmap may have resubmitted the slot meanwhile. */
   if (s->in_flight && s->pending_seqno == seqno &&
       s->pending_ring == pending_ring) {
      s->in_flight = false;
      s->pending_ring = NULL;
   }
}

/* Caller holds r->lock. */
static bool
slot_held_locked(const struct npt_d3d_map_ring *r, uint32_t slot)
{
   for (uint32_t i = 0; i < r->record_count; i++) {
      if (!r->records[i].in_place && r->records[i].slot == slot)
         return true;
   }
   return false;
}

/* Caller holds r->lock.  *wait: the slot is still in flight, and the
 * caller waits for it once its record holds the slot. */
static uint32_t
next_slot_locked(struct npt_d3d_map_ring *r, bool *wait)
{
   *wait = false;
   if (!r->slots || !r->active_count)
      return 0;

   const uint32_t cnt = r->active_count;
   uint32_t next = (r->current_slot + 1u) % cnt;
   if (!r->slots[next].in_flight && !slot_held_locked(r, next))
      return next;

   /* Grow path: bring up a fresh slot at active_count. */
   if (cnt < r->max_slots && alloc_slot_locked(r, cnt))
      return cnt;

   /* At cap (or grow failed): the first slot no open Map holds, waiting
    * for the host there. */
   for (uint32_t i = 0; i < cnt && slot_held_locked(r, next); i++)
      next = (next + 1u) % cnt;
   *wait = r->slots[next].in_flight;
   return next;
}

/* Caller holds r->lock. */
static struct npt_d3d_map_record *
find_record_locked(struct npt_d3d_map_ring *r, const void *context,
                   uint32_t subresource)
{
   for (uint32_t i = 0; i < r->record_count; i++) {
      if (r->records[i].context == context &&
          r->records[i].subresource == subresource)
         return &r->records[i];
   }
   return NULL;
}

bool
npt_d3d_map_ring_open(struct npt_d3d_map_ring *r,
                      struct npt_d3d_map_record *rec,
                      enum npt_d3d_map_slot_choice choice)
{
   mtx_lock(&r->lock);
   /* A Map with no Unmap is replaced by the new one. */
   struct npt_d3d_map_record *slot_rec =
      find_record_locked(r, rec->context, rec->subresource);
   if (!slot_rec) {
      if (r->record_count == r->record_cap) {
         const uint32_t cap = r->record_cap ? r->record_cap * 2u : 2u;
         struct npt_d3d_map_record *records =
            realloc(r->records, cap * sizeof(*records));
         if (!records) {
            mtx_unlock(&r->lock);
            return false;
         }
         r->records = records;
         r->record_cap = cap;
      }
      slot_rec = &r->records[r->record_count++];
   }
   /* Holds no slot until the choice below is made. */
   slot_rec->in_place = true;

   bool wait = false;
   switch (choice) {
   case NPT_D3D_MAP_SLOT_ZERO:
      rec->slot = 0;
      r->current_slot = 0;
      break;
   case NPT_D3D_MAP_SLOT_CURRENT:
      rec->slot = r->current_slot;
      break;
   case NPT_D3D_MAP_SLOT_NEXT:
      rec->slot = next_slot_locked(r, &wait);
      r->current_slot = rec->slot;
      break;
   case NPT_D3D_MAP_SLOT_SYNC:
      if (slot_held_locked(r, 0)) {
         rec->slot = next_slot_locked(r, &wait);
      } else {
         rec->slot = 0;
         wait = r->slots && r->slots[0].in_flight;
      }
      r->current_slot = rec->slot;
      break;
   }
   *slot_rec = *rec;
   /* The record holds the slot, so a rotate during the wait passes it. */
   if (wait)
      wait_slot_locked(r, rec->slot);
   mtx_unlock(&r->lock);
   return true;
}

void
npt_d3d_map_ring_set_byte_size(struct npt_d3d_map_ring *r,
                               const void *context, uint32_t subresource,
                               uint32_t byte_size)
{
   mtx_lock(&r->lock);
   struct npt_d3d_map_record *rec =
      find_record_locked(r, context, subresource);
   if (rec)
      rec->byte_size = byte_size;
   mtx_unlock(&r->lock);
}

bool
npt_d3d_map_ring_find(struct npt_d3d_map_ring *r, const void *context,
                      uint32_t subresource, struct npt_d3d_map_record *out)
{
   mtx_lock(&r->lock);
   const struct npt_d3d_map_record *rec =
      find_record_locked(r, context, subresource);
   if (rec)
      *out = *rec;
   mtx_unlock(&r->lock);
   return rec != NULL;
}

void
npt_d3d_map_ring_close(struct npt_d3d_map_ring *r, const void *context,
                       uint32_t subresource, struct npt_ring *ring,
                       uint32_t seqno)
{
   mtx_lock(&r->lock);
   struct npt_d3d_map_record *rec =
      find_record_locked(r, context, subresource);
   if (rec) {
      if (ring && rec->slot < r->active_count) {
         struct npt_d3d_map_slot *s = &r->slots[rec->slot];
         s->pending_seqno = seqno;
         s->pending_ring = ring;
         s->in_flight = true;
      }
      *rec = r->records[--r->record_count];
   }
   mtx_unlock(&r->lock);
}

void
npt_d3d_map_ring_slot_visible(const struct npt_d3d_map_ring *r, uint32_t slot,
                              struct npt_ring *ring)
{
   if (r->slots && slot < r->max_slots && r->slots[slot].shmem)
      npt_ring_ensure_shmem_visible(ring, r->slots[slot].shmem);
}

void
npt_d3d_map_ring_wait_slot(struct npt_d3d_map_ring *r, uint32_t slot)
{
   mtx_lock(&r->lock);
   if (r->slots && slot < r->active_count)
      wait_slot_locked(r, slot);
   mtx_unlock(&r->lock);
}

/* ==========================================================================
 * ID3D11Buffer wrapper
 * ========================================================================== */

static inline struct npt_d3d11_buffer_aux *
buf_aux(const struct npt_d3d11_buffer *b)
{
   return b ? ((struct npt_com_base *)b)->aux : NULL;
}

static void
npt_d3d11_buffer_aux_destroy(void *aux_raw)
{
   struct npt_d3d11_buffer_aux *aux = aux_raw;
   /* The host holds its own reference to each shmem's memory, so the
    * blobs need not outlive the COM_RELEASE queued before this. */
   for (uint32_t i = 0; i < NPT_D3D11_EXTERNAL_SLOT_MAX; i++) {
      if (aux->external_slots[i])
         npt_renderer_shmem_unref_async(aux->com->base.device->renderer,
                                        aux->external_slots[i]);
   }
   npt_d3d_map_ring_fini(&aux->map_ring);
   free(aux);
}

void
npt_d3d11_buffer_aux_init(struct npt_com_base *com,
                          struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d11_buffer_aux *aux = com->aux;
   aux->com = com;
   aux->external_current = NPT_EXTERNAL_COOKIE_NONE;
   npt_d3d_map_ring_init(&aux->map_ring, com);
   com->aux_destroy = npt_d3d11_buffer_aux_destroy;
   (void)dev; (void)host_id;
}

struct npt_d3d11_buffer *
npt_d3d11_buffer_cast(void *resource)
{
   if (!resource) return NULL;
   struct npt_com_base *com = resource;
   if (com->aux_destroy != npt_d3d11_buffer_aux_destroy) return NULL;
   return (struct npt_d3d11_buffer *)resource;
}

void
npt_d3d11_buffer_set_desc(struct npt_d3d11_buffer *b,
                          const D3D11_BUFFER_DESC *desc)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (!aux || !desc) return;
   aux->byte_width = desc->ByteWidth;
   aux->desc = *desc;
   aux->has_desc = true;
}

bool
npt_d3d11_buffer_fill_desc(struct npt_d3d11_buffer *b, D3D11_BUFFER_DESC *out)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (!aux || !aux->has_desc) return false;
   *out = aux->desc;
   return true;
}

uint32_t
npt_d3d11_buffer_get_byte_width(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? aux->byte_width : 0;
}

void
npt_d3d11_buffer_set_external_eligible(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux)
      atomic_store_explicit(&aux->external_eligible, true,
                            memory_order_relaxed);
}

bool
npt_d3d11_buffer_external_eligible(const struct npt_d3d11_buffer *b)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux && atomic_load_explicit(&aux->external_eligible,
                                      memory_order_relaxed);
}

void
npt_d3d11_buffer_retire_external(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux)
      atomic_store_explicit(&aux->external_retiring, true,
                            memory_order_relaxed);
}

bool
npt_d3d11_buffer_external_retiring(const struct npt_d3d11_buffer *b)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux && atomic_load_explicit(&aux->external_retiring,
                                      memory_order_relaxed);
}

void
npt_d3d11_buffer_drop_external(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (!aux)
      return;
   aux->external_current = NPT_EXTERNAL_COOKIE_NONE;
   atomic_store_explicit(&aux->external_eligible, false,
                         memory_order_relaxed);
}

bool
npt_d3d11_buffer_claim_external(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (!aux || aux->external_claimed)
      return false;
   aux->external_claimed = true;
   return true;
}

bool
npt_d3d11_buffer_external_claimed(const struct npt_d3d11_buffer *b)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux && aux->external_claimed;
}

uint32_t
npt_d3d11_buffer_external_current(const struct npt_d3d11_buffer *b)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? aux->external_current : NPT_EXTERNAL_COOKIE_NONE;
}

void
npt_d3d11_buffer_set_external_current(struct npt_d3d11_buffer *b,
                                      uint32_t cookie)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux)
      aux->external_current = cookie;
}

void *
npt_d3d11_buffer_external_slot_ptr(const struct npt_d3d11_buffer *b,
                                   uint32_t cookie)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (!aux || cookie >= NPT_D3D11_EXTERNAL_SLOT_MAX ||
       !aux->external_slots[cookie])
      return NULL;
   return aux->external_slots[cookie]->mmap_ptr;
}

/* Each slot pins a blob the size of the buffer for the buffer's
 * lifetime, so a large buffer gets fewer. */
static uint32_t
external_slot_cap(uint32_t byte_width)
{
   return byte_width <= (4u << 20)  ? NPT_D3D11_EXTERNAL_SLOT_MAX :
          byte_width <= (32u << 20) ? 4u : 3u;
}

bool
npt_d3d11_buffer_add_external_slot(struct npt_d3d11_buffer *b,
                                   struct npt_ring *ring)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (!aux || !ring || !aux->byte_width ||
       aux->external_reserved >= external_slot_cap(aux->byte_width))
      return false;

   const uint32_t cookie = aux->external_reserved++;

   /* The host wraps the blob as a no-copy GPU buffer, which wants a
    * whole number of its pages: 16 KiB on Apple silicon. */
   const size_t size = ((size_t)aux->byte_width + 16383u) & ~(size_t)16383u;
   struct npt_renderer *rndr = aux->com->base.device->renderer;
   struct npt_renderer_shmem *shmem = rndr->shmem_ops.create(rndr, size);
   if (!shmem) {
      /* With no shmem the host has nothing to rename onto. */
      if (cookie == 0)
         npt_d3d11_buffer_drop_external(b);
      return false;
   }

   /* The bind names the blob's res_id. */
   npt_ring_ensure_shmem_visible(ring, shmem);

   aux->external_slots[cookie] = shmem;
   const HRESULT hr = npt_dispatch_bind_d3d11_buffer_shmem(
      ring, aux->com->base.id, shmem->res_id, cookie, aux->byte_width);
   if (NPT_FAILED(hr)) {
      if (hr != NPT_E_NOTIMPL)
         npt_log("buffer 0x%llx: external slot %u bind failed 0x%08x",
                 (unsigned long long)aux->com->base.id, cookie,
                 (unsigned)hr);
      aux->external_slots[cookie] = NULL;
      npt_renderer_shmem_unref_async(rndr, shmem);
      if (cookie == 0)
         npt_d3d11_buffer_drop_external(b);
      return false;
   }
   return true;
}

void
npt_d3d11_buffer_wait_slot(struct npt_d3d11_buffer *b, uint32_t slot)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux)
      npt_d3d_map_ring_wait_slot(&aux->map_ring, slot);
}

bool
npt_d3d11_buffer_ensure_map_shmem(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (!aux || !aux->byte_width) return false;
   const uint32_t slot_size = (aux->byte_width + 63u) & ~63u;
   return npt_d3d_map_ring_alloc_shmem(&aux->map_ring, slot_size);
}

uint32_t
npt_d3d11_buffer_get_map_shmem_res_id(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0) : 0;
}

uint32_t
npt_d3d11_buffer_get_slot_shmem_res_id(struct npt_d3d11_buffer *b, uint32_t slot)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? npt_d3d_map_ring_slot_res_id(&aux->map_ring, slot) : 0;
}

bool
npt_d3d11_buffer_map_open(struct npt_d3d11_buffer *b, const void *context,
                          enum npt_d3d_map_slot_choice choice,
                          uint32_t access_flags, bool in_place,
                          uint32_t *out_slot)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (!aux)
      return false;
   struct npt_d3d_map_record rec = {
      .context = context,
      .access_flags = access_flags,
      .byte_size = aux->byte_width,
      .in_place = in_place,
   };
   if (!npt_d3d_map_ring_open(&aux->map_ring, &rec, choice))
      return false;
   *out_slot = rec.slot;
   return true;
}

bool
npt_d3d11_buffer_map_find(struct npt_d3d11_buffer *b, const void *context,
                          struct npt_d3d_map_record *out)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux && npt_d3d_map_ring_find(&aux->map_ring, context, 0, out);
}

void
npt_d3d11_buffer_map_close(struct npt_d3d11_buffer *b, const void *context,
                           struct npt_ring *ring, uint32_t seqno)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux)
      npt_d3d_map_ring_close(&aux->map_ring, context, 0, ring, seqno);
}

uint32_t
npt_d3d11_buffer_slot_offset(const struct npt_d3d11_buffer *b, uint32_t slot)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? npt_d3d_map_ring_slot_offset(&aux->map_ring, slot) : 0;
}

void
npt_d3d11_buffer_slot_visible(const struct npt_d3d11_buffer *b, uint32_t slot,
                              struct npt_ring *ring)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux)
      npt_d3d_map_ring_slot_visible(&aux->map_ring, slot, ring);
}

void *
npt_d3d11_buffer_slot_ptr(const struct npt_d3d11_buffer *b, uint32_t slot)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? npt_d3d_map_ring_slot_ptr(&aux->map_ring, slot) : NULL;
}

/* ==========================================================================
 * ID3D11Texture{1,2,3}D wrapper
 * ========================================================================== */

static inline struct npt_d3d11_texture_aux *
tex_aux(const struct npt_d3d11_texture *t)
{
   return t ? ((struct npt_com_base *)t)->aux : NULL;
}

static void
npt_d3d11_texture_aux_destroy(void *aux_raw)
{
   struct npt_d3d11_texture_aux *aux = aux_raw;
   npt_d3d_map_ring_fini(&aux->map_ring);
   free(aux);
}

void
npt_d3d11_texture_aux_init(struct npt_com_base *com,
                           struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d11_texture_aux *aux = com->aux;
   aux->com = com;
   npt_d3d_map_ring_init(&aux->map_ring, com);
   com->aux_destroy = npt_d3d11_texture_aux_destroy;
   aux->mip_levels = 1;
   aux->array_size = 1;
   aux->sample_count = 1;
   (void)dev; (void)host_id;
}

struct npt_d3d11_texture *
npt_d3d11_texture_cast(void *resource)
{
   if (!resource) return NULL;
   struct npt_com_base *com = resource;
   if (com->aux_destroy != npt_d3d11_texture_aux_destroy) return NULL;
   return (struct npt_d3d11_texture *)resource;
}

uint32_t
npt_dxgi_format_bytes_per_pixel(DXGI_FORMAT fmt)
{
   switch ((int)fmt) {
   case DXGI_FORMAT_R32G32B32A32_TYPELESS:
   case DXGI_FORMAT_R32G32B32A32_FLOAT:
   case DXGI_FORMAT_R32G32B32A32_UINT:
   case DXGI_FORMAT_R32G32B32A32_SINT:
      return 16;
   case DXGI_FORMAT_R32G32B32_TYPELESS:
   case DXGI_FORMAT_R32G32B32_FLOAT:
   case DXGI_FORMAT_R32G32B32_UINT:
   case DXGI_FORMAT_R32G32B32_SINT:
      return 12;
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
   case DXGI_FORMAT_R16G16B16A16_UNORM:
   case DXGI_FORMAT_R16G16B16A16_UINT:
   case DXGI_FORMAT_R16G16B16A16_SNORM:
   case DXGI_FORMAT_R16G16B16A16_SINT:
   case DXGI_FORMAT_R32G32_TYPELESS:
   case DXGI_FORMAT_R32G32_FLOAT:
   case DXGI_FORMAT_R32G32_UINT:
   case DXGI_FORMAT_R32G32_SINT:
   case DXGI_FORMAT_R32G8X24_TYPELESS:
   case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
   case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
   case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
      return 8;
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R10G10B10A2_UINT:
   case DXGI_FORMAT_R11G11B10_FLOAT:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_R8G8B8A8_UINT:
   case DXGI_FORMAT_R8G8B8A8_SNORM:
   case DXGI_FORMAT_R8G8B8A8_SINT:
   case DXGI_FORMAT_R16G16_TYPELESS:
   case DXGI_FORMAT_R16G16_FLOAT:
   case DXGI_FORMAT_R16G16_UNORM:
   case DXGI_FORMAT_R16G16_UINT:
   case DXGI_FORMAT_R16G16_SNORM:
   case DXGI_FORMAT_R16G16_SINT:
   case DXGI_FORMAT_R32_TYPELESS:
   case DXGI_FORMAT_D32_FLOAT:
   case DXGI_FORMAT_R32_FLOAT:
   case DXGI_FORMAT_R32_UINT:
   case DXGI_FORMAT_R32_SINT:
   case DXGI_FORMAT_R24G8_TYPELESS:
   case DXGI_FORMAT_D24_UNORM_S8_UINT:
   case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
   case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
   case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
   case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
      return 4;
   case DXGI_FORMAT_R8G8_TYPELESS:
   case DXGI_FORMAT_R8G8_UNORM:
   case DXGI_FORMAT_R8G8_UINT:
   case DXGI_FORMAT_R8G8_SNORM:
   case DXGI_FORMAT_R8G8_SINT:
   case DXGI_FORMAT_R16_TYPELESS:
   case DXGI_FORMAT_R16_FLOAT:
   case DXGI_FORMAT_D16_UNORM:
   case DXGI_FORMAT_R16_UNORM:
   case DXGI_FORMAT_R16_UINT:
   case DXGI_FORMAT_R16_SNORM:
   case DXGI_FORMAT_R16_SINT:
   case DXGI_FORMAT_B5G6R5_UNORM:
   case DXGI_FORMAT_B5G5R5A1_UNORM:
   case DXGI_FORMAT_B4G4R4A4_UNORM:
   case DXGI_FORMAT_A8P8:
   case 191: /* DXGI_FORMAT_A4B4G4R4_UNORM (Windows 11 24H2 SDK) */
      return 2;
   case DXGI_FORMAT_R8_TYPELESS:
   case DXGI_FORMAT_R8_UNORM:
   case DXGI_FORMAT_R8_UINT:
   case DXGI_FORMAT_R8_SNORM:
   case DXGI_FORMAT_R8_SINT:
   case DXGI_FORMAT_A8_UNORM:
   case DXGI_FORMAT_P8:
   case DXGI_FORMAT_AI44:
   case DXGI_FORMAT_IA44:
      return 1;
   default:
      return 0;
   }
}

static uint32_t
format_block_height(DXGI_FORMAT fmt)
{
   switch ((int)fmt) {
   case DXGI_FORMAT_BC1_TYPELESS:
   case DXGI_FORMAT_BC1_UNORM:
   case DXGI_FORMAT_BC1_UNORM_SRGB:
   case DXGI_FORMAT_BC2_TYPELESS:
   case DXGI_FORMAT_BC2_UNORM:
   case DXGI_FORMAT_BC2_UNORM_SRGB:
   case DXGI_FORMAT_BC3_TYPELESS:
   case DXGI_FORMAT_BC3_UNORM:
   case DXGI_FORMAT_BC3_UNORM_SRGB:
   case DXGI_FORMAT_BC4_TYPELESS:
   case DXGI_FORMAT_BC4_UNORM:
   case DXGI_FORMAT_BC4_SNORM:
   case DXGI_FORMAT_BC5_TYPELESS:
   case DXGI_FORMAT_BC5_UNORM:
   case DXGI_FORMAT_BC5_SNORM:
   case DXGI_FORMAT_BC6H_TYPELESS:
   case DXGI_FORMAT_BC6H_UF16:
   case DXGI_FORMAT_BC6H_SF16:
   case DXGI_FORMAT_BC7_TYPELESS:
   case DXGI_FORMAT_BC7_UNORM:
   case DXGI_FORMAT_BC7_UNORM_SRGB:
      return 4;
   default:
      return 1;
   }
}

uint32_t
npt_dxgi_format_block_rows(DXGI_FORMAT fmt, uint32_t texel_rows)
{
   const uint32_t bh = format_block_height(fmt);
   return (texel_rows + bh - 1u) / bh;
}

uint32_t
npt_dxgi_format_row_texels(DXGI_FORMAT fmt)
{
   return format_block_height(fmt);
}

static bool
update_send(struct npt_ring *ring, uint64_t context_id, uint64_t resource_id,
            uint32_t subresource,
            const D3D11_BOX *box, uint32_t row_pitch, uint32_t depth_pitch,
            const uint8_t *data, uint64_t full, uint64_t copy)
{
   return npt_dispatch_resource_update(ring, context_id, resource_id,
                                       subresource,
                                       row_pitch, depth_pitch, box, data,
                                       (uint32_t)full, (uint32_t)copy);
}

bool
npt_resource_update(struct npt_ring *ring, uint64_t context_id,
                    uint64_t resource_id,
                    uint32_t subresource, const D3D11_BOX *box,
                    uint32_t row_pitch, uint32_t depth_pitch,
                    const void *data, uint64_t full, uint64_t copy,
                    const struct npt_update_split *split)
{
   if (!full || !copy || copy > full)
      return false;
   if (full <= NPT_RESOURCE_UPDATE_CHUNK_BYTES)
      return update_send(ring, context_id, resource_id, subresource, box, row_pitch,
                         depth_pitch, data, full, copy);

   const uint64_t cap = NPT_RESOURCE_UPDATE_CHUNK_BYTES;
   const uint8_t *src = data;
   /* Only the region's last row may end short of a full pitch, so every
    * band but the final one reads its whole footprint. */
   const uint64_t tail = full - copy;
   bool ok = true;

   if (split && split->buffer) {
      const uint32_t left = split->extent.left;
      const uint32_t right = split->extent.right;
      for (uint32_t l = left; l < right && ok; ) {
         const uint32_t r = right - l > cap ? l + (uint32_t)cap : right;
         const D3D11_BOX b = { l, 0, 0, r, 1, 1 };
         ok = update_send(ring, context_id, resource_id, subresource, &b, 0, 0,
                          src + (l - left), r - l, r - l);
         l = r;
      }
      return ok;
   }

   if (!split || !row_pitch || !split->rows || !split->row_texels) {
      npt_log("resource_update: %llu-byte transfer to 0x%llx sub %u has no "
              "band layout -- dropped",
              (unsigned long long)full, (unsigned long long)resource_id,
              subresource);
      return false;
   }

   const D3D11_BOX *e = &split->extent;
   const uint32_t slices = e->back - e->front;
   const uint64_t slice_bytes = (uint64_t)row_pitch * split->rows;

   if (slices > 1 && depth_pitch >= slice_bytes && slice_bytes <= cap) {
      const uint32_t per = 1u + (uint32_t)((cap - slice_bytes) / depth_pitch);
      for (uint32_t s0 = 0; s0 < slices && ok; s0 += per) {
         const uint32_t n = slices - s0 < per ? slices - s0 : per;
         const uint64_t band = (uint64_t)depth_pitch * (n - 1u) + slice_bytes;
         const bool last = s0 + n == slices;
         D3D11_BOX b = *e;
         b.front = e->front + s0;
         b.back = b.front + n;
         ok = update_send(ring, context_id, resource_id, subresource, &b, row_pitch,
                          depth_pitch, src + (uint64_t)depth_pitch * s0, band,
                          last ? band - tail : band);
      }
      return ok;
   }

   uint32_t per = (uint32_t)(cap / row_pitch);
   if (!per)
      per = 1;
   for (uint32_t s = 0; s < (slices ? slices : 1u) && ok; s++) {
      for (uint32_t r0 = 0; r0 < split->rows && ok; r0 += per) {
         const uint32_t n = split->rows - r0 < per ? split->rows - r0 : per;
         const uint64_t band = (uint64_t)row_pitch * n;
         const bool last = s + 1u >= slices && r0 + n == split->rows;
         D3D11_BOX b = *e;
         b.front = e->front + s;
         b.back = b.front + 1u;
         b.top = e->top + r0 * split->row_texels;
         const uint32_t bottom = e->top + (r0 + n) * split->row_texels;
         b.bottom = bottom < e->bottom ? bottom : e->bottom;
         ok = update_send(ring, context_id, resource_id, subresource, &b, row_pitch,
                          depth_pitch,
                          src + (uint64_t)depth_pitch * s +
                             (uint64_t)row_pitch * r0,
                          band, last ? band - tail : band);
      }
   }
   return ok;
}

/* Bytes actually occupied by one row of blocks: the tight
 * RowSizeInBytes of D3D's UpdateSubresource source contract.
 * Returns 0 when the format's size is unknown (callers keep
 * their legacy full-pitch sizing in that case). */
uint32_t
npt_dxgi_format_row_bytes(DXGI_FORMAT fmt, uint32_t texel_width)
{
   switch ((int)fmt) {
   case DXGI_FORMAT_BC1_TYPELESS:
   case DXGI_FORMAT_BC1_UNORM:
   case DXGI_FORMAT_BC1_UNORM_SRGB:
   case DXGI_FORMAT_BC4_TYPELESS:
   case DXGI_FORMAT_BC4_UNORM:
   case DXGI_FORMAT_BC4_SNORM:
      return ((texel_width + 3u) / 4u) * 8u;
   case DXGI_FORMAT_BC2_TYPELESS:
   case DXGI_FORMAT_BC2_UNORM:
   case DXGI_FORMAT_BC2_UNORM_SRGB:
   case DXGI_FORMAT_BC3_TYPELESS:
   case DXGI_FORMAT_BC3_UNORM:
   case DXGI_FORMAT_BC3_UNORM_SRGB:
   case DXGI_FORMAT_BC5_TYPELESS:
   case DXGI_FORMAT_BC5_UNORM:
   case DXGI_FORMAT_BC5_SNORM:
   case DXGI_FORMAT_BC6H_TYPELESS:
   case DXGI_FORMAT_BC6H_UF16:
   case DXGI_FORMAT_BC6H_SF16:
   case DXGI_FORMAT_BC7_TYPELESS:
   case DXGI_FORMAT_BC7_UNORM:
   case DXGI_FORMAT_BC7_UNORM_SRGB:
      return ((texel_width + 3u) / 4u) * 16u;
   case DXGI_FORMAT_R8G8_B8G8_UNORM:
   case DXGI_FORMAT_G8R8_G8B8_UNORM:
   case DXGI_FORMAT_YUY2:
      /* Packed pairs: 32 bits per two texels. */
      return ((texel_width + 1u) / 2u) * 4u;
   case DXGI_FORMAT_Y210:
   case DXGI_FORMAT_Y216:
      /* Packed pairs: 64 bits per two texels. */
      return ((texel_width + 1u) / 2u) * 8u;
   case DXGI_FORMAT_AYUV:
   case DXGI_FORMAT_Y410:
      return texel_width * 4u;
   case DXGI_FORMAT_Y416:
      return texel_width * 8u;
   case DXGI_FORMAT_R1_UNORM:
      return (texel_width + 7u) / 8u;
   case DXGI_FORMAT_NV12:
   case DXGI_FORMAT_NV11:
   case DXGI_FORMAT_420_OPAQUE:
   case DXGI_FORMAT_P208:
      /* Planar 8-bit: one luma byte per texel; the chroma plane(s) are
       * extra rows of the same pitch (npt_dxgi_format_subresource_rows). */
      return texel_width;
   case DXGI_FORMAT_P010:
   case DXGI_FORMAT_P016:
      return texel_width * 2u;
   default:
      return texel_width * npt_dxgi_format_bytes_per_pixel(fmt);
   }
}

uint32_t
npt_dxgi_format_subresource_rows(DXGI_FORMAT fmt, uint32_t height)
{
   switch ((int)fmt) {
   case DXGI_FORMAT_NV12:
   case DXGI_FORMAT_P010:
   case DXGI_FORMAT_P016:
   case DXGI_FORMAT_420_OPAQUE:
      /* Luma plane, then a half-height interleaved chroma plane. */
      return height + height / 2u;
   case DXGI_FORMAT_NV11:
      /* Luma plane, then a half-pitch full-height chroma plane, then
       * padding out to twice the luma plane. */
      return height * 2u;
   default:
      /* P208 / V208 / V408 are multi-plane too but their initial-data
       * footprint is undocumented; one row per texel row under-copies
       * rather than reading past the app's buffer. */
      return npt_dxgi_format_block_rows(fmt, height);
   }
}

uint32_t
npt_d3d11_texture_desc_mip_levels(const struct npt_d3d11_texture_desc *d)
{
   if (d->mip_levels)
      return d->mip_levels;
   /* MipLevels 0 asks for the full chain, down to 1x1x1. */
   uint32_t extent = d->width;
   if (d->height > extent) extent = d->height;
   if (d->depth > extent) extent = d->depth;
   uint32_t levels = 1;
   while (extent > 1) {
      extent >>= 1;
      levels++;
   }
   return levels;
}

void
npt_d3d11_texture_set_desc(struct npt_d3d11_texture *t,
                           const struct npt_d3d11_texture_desc *d)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !d) return;
   aux->width = d->width;
   aux->height = d->height ? d->height : 1;
   aux->depth = d->depth ? d->depth : 1;
   aux->mip_levels = npt_d3d11_texture_desc_mip_levels(d);
   aux->array_size = d->array_size ? d->array_size : 1;
   aux->format = d->format;
   aux->bytes_per_pixel = npt_dxgi_format_bytes_per_pixel(d->format);
   aux->usage = d->usage;
   aux->bind_flags = d->bind_flags;
   aux->cpu_access_flags = d->cpu_access_flags;
   aux->misc_flags = d->misc_flags;
   aux->sample_count = d->sample_count ? d->sample_count : 1;
   aux->sample_quality = d->sample_quality;
   aux->texture_layout = d->texture_layout;
   aux->has_desc = true;
}

void
npt_d3d11_texture_fill_desc1d(const struct npt_d3d11_texture *t,
                              D3D11_TEXTURE1D_DESC *out)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !out) return;
   out->Width = aux->width;
   out->MipLevels = aux->mip_levels;
   out->ArraySize = aux->array_size;
   out->Format = aux->format;
   out->Usage = (D3D11_USAGE)aux->usage;
   out->BindFlags = aux->bind_flags;
   out->CPUAccessFlags = aux->cpu_access_flags;
   out->MiscFlags = aux->misc_flags;
}

void
npt_d3d11_texture_fill_desc2d(const struct npt_d3d11_texture *t,
                              D3D11_TEXTURE2D_DESC *out)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !out) return;
   out->Width = aux->width;
   out->Height = aux->height;
   out->MipLevels = aux->mip_levels;
   out->ArraySize = aux->array_size;
   out->Format = aux->format;
   out->SampleDesc.Count = aux->sample_count;
   out->SampleDesc.Quality = aux->sample_quality;
   out->Usage = (D3D11_USAGE)aux->usage;
   out->BindFlags = aux->bind_flags;
   out->CPUAccessFlags = aux->cpu_access_flags;
   out->MiscFlags = aux->misc_flags;
}

void
npt_d3d11_texture_fill_desc2d1(const struct npt_d3d11_texture *t,
                               D3D11_TEXTURE2D_DESC1 *out)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !out) return;
   out->Width = aux->width;
   out->Height = aux->height;
   out->MipLevels = aux->mip_levels;
   out->ArraySize = aux->array_size;
   out->Format = aux->format;
   out->SampleDesc.Count = aux->sample_count;
   out->SampleDesc.Quality = aux->sample_quality;
   out->Usage = (D3D11_USAGE)aux->usage;
   out->BindFlags = aux->bind_flags;
   out->CPUAccessFlags = aux->cpu_access_flags;
   out->MiscFlags = aux->misc_flags;
   out->TextureLayout = aux->texture_layout;
}

void
npt_d3d11_texture_fill_desc3d(const struct npt_d3d11_texture *t,
                              D3D11_TEXTURE3D_DESC *out)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !out) return;
   out->Width = aux->width;
   out->Height = aux->height;
   out->Depth = aux->depth;
   out->MipLevels = aux->mip_levels;
   out->Format = aux->format;
   out->Usage = (D3D11_USAGE)aux->usage;
   out->BindFlags = aux->bind_flags;
   out->CPUAccessFlags = aux->cpu_access_flags;
   out->MiscFlags = aux->misc_flags;
}

void
npt_d3d11_texture_fill_desc3d1(const struct npt_d3d11_texture *t,
                               D3D11_TEXTURE3D_DESC1 *out)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !out) return;
   out->Width = aux->width;
   out->Height = aux->height;
   out->Depth = aux->depth;
   out->MipLevels = aux->mip_levels;
   out->Format = aux->format;
   out->Usage = (D3D11_USAGE)aux->usage;
   out->BindFlags = aux->bind_flags;
   out->CPUAccessFlags = aux->cpu_access_flags;
   out->MiscFlags = aux->misc_flags;
   out->TextureLayout = aux->texture_layout;
}

bool
npt_d3d11_texture_has_desc(const struct npt_d3d11_texture *t)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux && aux->has_desc;
}

bool
npt_d3d11_texture_is_mappable(const struct npt_d3d11_texture *t)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return false;
   if (aux->width == 0 || aux->height == 0 || aux->depth == 0 ||
       npt_dxgi_format_row_bytes(aux->format, aux->width) == 0 ||
       npt_dxgi_format_subresource_rows(aux->format, aux->height) == 0)
      return false;
   if (aux->usage == D3D11_USAGE_DYNAMIC)
      return (aux->cpu_access_flags & D3D11_CPU_ACCESS_WRITE) != 0;
   /* Staging textures, and default textures created with CPU access:
    * the UMD reports UMA, so the runtime allows Map on the latter
    * (D3D11_FEATURE_DATA_D3D11_OPTIONS2.MapOnDefaultTextures) and the HLK
    * substitutes them for staging copies. */
   if (aux->usage == D3D11_USAGE_STAGING || aux->usage == D3D11_USAGE_DEFAULT)
      return (aux->cpu_access_flags &
              (D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ)) != 0;
   return false;
}

static inline uint32_t
mip_dim(uint32_t base, uint32_t mip)
{
   return base > (1u << mip) ? (base >> mip) : 1u;
}

static uint32_t
texture_subresource_mip(const struct npt_d3d11_texture_aux *aux, uint32_t subresource)
{
   const uint32_t mips = aux->mip_levels ? aux->mip_levels : 1u;
   return subresource % mips;
}

/* Footprint of one mapped subresource at the host's pitches: every
 * row of memory charged at the full row pitch, and every slice but the
 * last at the full depth pitch (a depth pitch below one slice's rows
 * is treated as unpadded).  This is the region a Map hands out and an
 * Unmap transfers. */
uint32_t
npt_d3d11_texture_get_subresource_byte_size(const struct npt_d3d11_texture *t,
                                            uint32_t subresource,
                                            uint32_t row_pitch,
                                            uint32_t depth_pitch)
{
   uint32_t rows = 0, d = 0;
   npt_d3d11_texture_get_map_extent(t, subresource, &rows, &d);
   if (!rows || !d) return 0;
   const uint64_t slice = (uint64_t)row_pitch * rows;
   const uint64_t strides = (d > 1u && depth_pitch >= slice)
      ? (uint64_t)depth_pitch * (d - 1u)
      : slice * (d - 1u);
   const uint64_t size = strides + slice;
   /* 0 also means "cannot size this" to both callers. */
   return size && size <= 0xffffffffu ? (uint32_t)size : 0;
}

/* Rows of memory and depth slices in one subresource: block rows for
 * the compressed families, luma + chroma rows for the planar ones. */
void
npt_d3d11_texture_get_map_extent(const struct npt_d3d11_texture *t,
                                 uint32_t subresource,
                                 uint32_t *out_rows, uint32_t *out_depth)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   *out_rows = 0;
   *out_depth = 0;
   if (!aux) return;
   const uint32_t mip = texture_subresource_mip(aux, subresource);
   *out_rows = npt_dxgi_format_subresource_rows(aux->format,
                                                mip_dim(aux->height, mip));
   *out_depth = mip_dim(aux->depth, mip);
}

/* Sizes of a no-box UpdateSubresource transfer.  The return value is
 * the full-pitch footprint the host's UpdateSubresource may consume
 * (the ring reservation); *out_copy_size is the extent D3D guarantees
 * readable in the caller's buffer -- (rows-1)*pitch + RowSizeInBytes
 * per slice -- which is all the guest may copy.  Planar formats copy
 * the full footprint: MSDN documents their initial-data and staging
 * layout as rowPitch * subresource_rows with no final-row discount.
 * Returns 0 (with *out_copy_size = 0) when the transfer cannot be
 * sized. */
uint64_t
npt_d3d11_texture_get_subresource_update_sizes(const struct npt_d3d11_texture *t,
                                               uint32_t subresource,
                                               uint32_t row_pitch,
                                               uint32_t depth_pitch,
                                               uint64_t *out_copy_size,
                                               struct npt_update_split *out_split,
                                               bool *split_ok)
{
   *out_copy_size = 0;
   *split_ok = false;
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return 0;
   const uint32_t mip = texture_subresource_mip(aux, subresource);
   const uint32_t mw  = mip_dim(aux->width, mip);
   const uint32_t mh  = mip_dim(aux->height, mip);
   const uint32_t d   = mip_dim(aux->depth, mip);
   const uint32_t rows = npt_dxgi_format_subresource_rows(aux->format, mh);
   const uint32_t last_row = npt_dxgi_format_row_bytes(aux->format, mw);
   const bool tight = last_row &&
      rows == npt_dxgi_format_block_rows(aux->format, mh);
   const uint64_t slice = (uint64_t)row_pitch * rows;
   const uint64_t strides = (d > 1u && depth_pitch)
      ? (uint64_t)depth_pitch * (d - 1u)
      : slice * (d - 1u);
   const uint64_t full = strides + slice;
   uint64_t copy = tight
      ? strides + (uint64_t)row_pitch * (rows - 1u) + last_row
      : full;
   if (copy > full)
      copy = full;
   if (!full)
      return 0;
   *out_copy_size = copy;
   const D3D11_BOX extent = { 0, 0, 0, mw, mh, d };
   out_split->extent = extent;
   out_split->rows = rows;
   out_split->row_texels = npt_dxgi_format_row_texels(aux->format);
   out_split->buffer = false;
   *split_ok = rows == npt_dxgi_format_block_rows(aux->format, mh) &&
               (d <= 1u || depth_pitch);
   return full;
}

void
npt_d3d11_texture_get_mip_dimensions(const struct npt_d3d11_texture *t,
                                     uint32_t subresource,
                                     uint32_t *out_height,
                                     uint32_t *out_depth)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) {
      if (out_height) *out_height = 0;
      if (out_depth)  *out_depth  = 0;
      return;
   }
   const uint32_t mip = texture_subresource_mip(aux, subresource);
   if (out_height) *out_height = mip_dim(aux->height, mip);
   if (out_depth)  *out_depth  = mip_dim(aux->depth,  mip);
}

uint32_t
npt_d3d11_texture_get_bytes_per_pixel(const struct npt_d3d11_texture *t)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux ? aux->bytes_per_pixel : 0;
}

DXGI_FORMAT
npt_d3d11_texture_get_format(const struct npt_d3d11_texture *t)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux ? aux->format : DXGI_FORMAT_UNKNOWN;
}

bool
npt_d3d11_texture_ensure_map_shmem(struct npt_d3d11_texture *t)
{
   if (!npt_d3d11_texture_is_mappable(t)) return false;
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return false;
   /* Mip 0 of the largest subresource, with the row pitch rounded up
    * to the 256-byte alignment the host backends stay within. */
   const uint32_t row_bytes =
      npt_dxgi_format_row_bytes(aux->format, aux->width);
   const uint32_t rows =
      npt_dxgi_format_subresource_rows(aux->format, aux->height);
   const uint32_t aligned_row_pitch = (row_bytes + 255u) & ~255u;
   const uint64_t per_slot = (uint64_t)aligned_row_pitch *
                             (uint64_t)rows *
                             (uint64_t)aux->depth;
   const uint64_t aligned_slot = (per_slot + 63u) & ~(uint64_t)63u;
   /* Per-slot cap: 2 GB, the largest a slot's 32-bit size carries (a
    * 16384 x 16384 subresource of a 16-byte format is 4 GB and cannot be
    * mapped); slots are lazily allocated up to the ring's cap. */
   if (per_slot == 0 || aligned_slot > (uint64_t)(2048u << 20)) {
      npt_log("texture_ensure_map_shmem: bad size");
      return false;
   }
   return npt_d3d_map_ring_alloc_shmem(&aux->map_ring, (uint32_t)aligned_slot);
}

uint32_t npt_d3d11_texture_get_slot_shmem_res_id(const struct npt_d3d11_texture *t,
                                                 uint32_t slot)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? npt_d3d_map_ring_slot_res_id(&aux->map_ring, slot) : 0; }

uint32_t npt_d3d11_texture_get_shmem_size(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->map_ring.aligned_slot_size * aux->map_ring.active_count : 0; }

uint32_t npt_d3d11_texture_get_slot_size(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->map_ring.aligned_slot_size : 0; }

uint32_t npt_d3d11_texture_slot_offset(const struct npt_d3d11_texture *t, uint32_t slot)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? npt_d3d_map_ring_slot_offset(&aux->map_ring, slot) : 0; }

void npt_d3d11_texture_slot_visible(const struct npt_d3d11_texture *t,
                                    uint32_t slot, struct npt_ring *ring)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (aux)
      npt_d3d_map_ring_slot_visible(&aux->map_ring, slot, ring);
}

void *npt_d3d11_texture_slot_ptr(const struct npt_d3d11_texture *t, uint32_t slot)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux ? npt_d3d_map_ring_slot_ptr(&aux->map_ring, slot) : NULL;
}

bool npt_d3d11_texture_map_open(struct npt_d3d11_texture *t,
                                const void *context, uint32_t subresource,
                                enum npt_d3d_map_slot_choice choice,
                                uint32_t access_flags, uint32_t byte_size,
                                uint32_t *out_slot)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux)
      return false;
   struct npt_d3d_map_record rec = {
      .context = context,
      .subresource = subresource,
      .access_flags = access_flags,
      .byte_size = byte_size,
   };
   if (!npt_d3d_map_ring_open(&aux->map_ring, &rec, choice))
      return false;
   *out_slot = rec.slot;
   return true;
}

bool npt_d3d11_texture_map_find(struct npt_d3d11_texture *t,
                                const void *context, uint32_t subresource,
                                struct npt_d3d_map_record *out)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux && npt_d3d_map_ring_find(&aux->map_ring, context, subresource,
                                       out);
}

void npt_d3d11_texture_map_set_byte_size(struct npt_d3d11_texture *t,
                                         const void *context,
                                         uint32_t subresource,
                                         uint32_t byte_size)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (aux)
      npt_d3d_map_ring_set_byte_size(&aux->map_ring, context, subresource,
                                     byte_size);
}

void npt_d3d11_texture_map_close(struct npt_d3d11_texture *t,
                                 const void *context, uint32_t subresource,
                                 struct npt_ring *ring, uint32_t seqno)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (aux)
      npt_d3d_map_ring_close(&aux->map_ring, context, subresource, ring,
                             seqno);
}

uint32_t npt_d3d11_texture_get_cached_row_pitch(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->cached_row_pitch : 0; }
uint32_t npt_d3d11_texture_get_cached_depth_pitch(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->cached_depth_pitch : 0; }

void npt_d3d11_texture_set_cached_pitches(struct npt_d3d11_texture *t,
                                          uint32_t row_pitch, uint32_t depth_pitch)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return;
   aux->cached_row_pitch = row_pitch;
   aux->cached_depth_pitch = depth_pitch;
}
