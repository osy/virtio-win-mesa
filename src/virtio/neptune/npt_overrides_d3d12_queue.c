/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12CommandQueue overrides.
 *
 * ExecuteCommandLists: under multi-ring, the calling thread's device-
 * method traffic (descriptor writes, resource updates) rides its TLS
 * ring while the queue is pinned to the shared instance ring.  Drain
 * the caller's TLS ring first so everything the app recorded/wrote on
 * this thread is host-visible before the submission dispatches.
 *
 * Cross-thread recording (lists recorded + Closed on other threads'
 * rings) is fenced per list: async Close stamps {ring id, seqno} in
 * the list aux (npt_overrides_d3d12_list.c) and each stamp is waited
 * to its seqno here.  A list without a stamp (QI-alias wrapper, aux
 * OOM) falls back to draining every ring.
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_ring.h"
#include "npt_tls.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_client_id3d12commandqueue.h"
#include "neptune-protocol/npt_protocol_guest_id3d12commandqueue.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* npt_overrides_d3d12_resource.c: push persistently-sync-mapped buffer
 * shadows to the host before the GPU consumes them.  Fire-and-forget:
 * the flush commands ride the queue ring so ring FIFO orders them
 * ahead of the ExecuteCommandLists submitted below. */
void npt_d3d12_sync_maps_flush(struct npt_ring *queue_ring);

/* npt_overrides_d3d12_fence.c: record a queued Signal value so a rewind
 * (Signal below the fence's high-water mark) disables the snapshot
 * fast paths. */
void npt_d3d12_fence_note_signal(void *fence_wrapper, UINT64 value);

static void NPT_STDMETHODCALLTYPE
queue12_ExecuteCommandLists_override(void *self, UINT NumCommandLists,
                                     ID3D12CommandList **ppCommandLists)
{
   struct npt_device *dev = npt_com_self_device(self);
   struct npt_ring *queue_ring = npt_com_self_ring(self);

   npt_d3d12_sync_maps_flush(queue_ring);

   if (dev->multi_ring_enabled) {
      /* Peek, don't create: a thread that only ever submits has nothing
       * of its own to drain, and creating a ring here would drain the
       * primary for nothing. */
      struct npt_ring *tls_ring = npt_tls_peek_ring(dev);
      if (tls_ring && tls_ring != queue_ring)
         npt_ring_wait_all(tls_ring);

      bool full_drain_done = false;
      for (UINT i = 0; i < NumCommandLists; i++) {
         uint64_t ring_id;
         uint32_t seqno;
         if (npt_d3d12_list_close_barrier(ppCommandLists[i], &ring_id,
                                          &seqno)) {
            /* The caller's TLS ring was just drained wholesale; only
             * other rings still need their Close fenced. */
            if (!full_drain_done && ring_id != queue_ring->id &&
                !(tls_ring && ring_id == tls_ring->id))
               npt_tls_wait_ring_seqno(dev, ring_id, seqno);
         } else if (!full_drain_done) {
            npt_tls_wait_all_rings(dev);
            full_drain_done = true;
         }
      }
   }

   struct npt_ring_submit_command submit;
   npt_submit_ID3D12CommandQueue_ExecuteCommandLists(
      queue_ring, 0, npt_com_self_id(self),
      NumCommandLists, ppCommandLists, &submit);

   /* Stamp the submission on every list so a Reset issued right after
    * -- which D3D12 permits -- waits the queue ring forward to this
    * Execute before its own async dispatch.  Otherwise the Reset can
    * overtake the Execute, the host decodes a list still in recording
    * state, and it marks the device removed. */
   for (UINT i = 0; i < NumCommandLists; i++)
      npt_d3d12_list_stamp_execute(ppCommandLists[i], queue_ring->id,
                                   submit.seqno);
}

static HRESULT NPT_STDMETHODCALLTYPE
queue12_Signal_override(void *self, ID3D12Fence *pFence, UINT64 Value)
{
   if (pFence)
      npt_d3d12_fence_note_signal(pFence, Value);
   return npt_id3d12commandqueue_default_Signal(self, pFence, Value);
}

void
npt_overrides_d3d12_queue_init(void)
{
   NPT_REGISTER_OVERRIDE(id3d12commandqueue, ExecuteCommandLists,
                         queue12_ExecuteCommandLists_override);
   NPT_REGISTER_OVERRIDE(id3d12commandqueue, Signal,
                         queue12_Signal_override);
}
