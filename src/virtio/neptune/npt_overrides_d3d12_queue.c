/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12CommandQueue overrides.
 *
 * ExecuteCommandLists: the submission consumes everything the app did
 * before calling it -- the listed lists' recording and Close on their
 * recording threads, descriptor writes and resource updates on
 * whichever threads made them -- and the app's own synchronisation
 * ordered all of that before this call.  On the single ring, threads
 * hold such commands in staging buffers, so every stage is published
 * first and ring FIFO does the rest.  Multi-ring: an ordering edge on
 * the queue ring after every other ring's current tail makes the host
 * decode the submission after all of it, which is the same FIFO, while
 * the rings keep decoding in parallel and this thread never waits.
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

   npt_ring_stage_flush_all(queue_ring);
   npt_d3d12_sync_maps_flush(queue_ring);

   if (dev->multi_ring_enabled)
      npt_ring_order_after_all(queue_ring);

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
