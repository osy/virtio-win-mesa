/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D12Fence{,1} overrides:
 *  - SetEventOnCompletion is skip_default: the NULL-event form blocks
 *    the calling thread on a sync round trip that the host waits
 *    out, and the event form pre-arms the host eventfd proxy so the
 *    host signals it on GPU completion.
 *  - GetCompletedValue reads the per-fence feedback slot (plain
 *    snapshot; D3D12 fences may be rewound, so the value is whatever
 *    the host last observed, not a monotonic max).
 *  - Device::CreateFence defers to the default thunk, then seeds +
 *    registers the feedback slot.
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "npt_com.h"
#include "npt_overrides_d3d11_feedback.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_env.h"
#include "npt_event.h"
#include "npt_overrides.h"
#include "npt_ring.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_client_id3d12device.h"
#include "neptune-protocol/npt_protocol_client_id3d12fence.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_guest_id3d12fence.h"

#define NPT_REGISTER_OVERRIDE_D3D12_FENCE1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12fence1, m, f)
#define NPT_REGISTER_OVERRIDE_D3D12_FENCE(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12fence, m, f); \
   NPT_REGISTER_OVERRIDE_D3D12_FENCE1(m, f)

/* CreateFence cascade over every device tier. */
#define NPT_REGISTER_OVERRIDE_D3D12_DEVICE_ALL(m, f) \
   NPT_REGISTER_OVERRIDE(id3d12device, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device1, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device2, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device3, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device4, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device5, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device6, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device7, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device8, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device9, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device10, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device11, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device12, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device13, m, f); \
   NPT_REGISTER_OVERRIDE(id3d12device14, m, f)

static const GUID *const fence12_tiers[] = {
   &NPT_IID_ID3D12Fence, &NPT_IID_ID3D12Fence1, NULL,
};

/* Slot layout shared with D3D11 (npt_d3d11_fence_feedback_slot). */
struct npt_d3d12_fence_aux {
   struct npt_d3d11_feedback_aux base;
   /* Highest value ever requested through Fence::Signal or
    * CommandQueue::Signal; used to detect a rewind (D3D12 permits
    * Signal to a lower value). */
   _Atomic uint64_t signal_high;
   /* Sticky once any Signal decreases below signal_high.  The feedback
    * slot is a non-monotonic snapshot, so after a rewind it can read
    * stale-high; a latching wait decision (SetEventOnCompletion) off
    * that snapshot would fire early.  Rewound fences bypass the
    * snapshot fast paths and use the host-authoritative arm / sync. */
   _Atomic bool rewound;
};

static void npt_d3d12_fence_aux_destroy(void *aux_raw);

static void
npt_d3d12_fence_aux_init(struct npt_com_base *com,
                         struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d12_fence_aux *aux = com->aux;
   aux->base.com = com;
   aux->base.fb_shmem = NULL;
   aux->base.fb_offset = 0;
   aux->base.registered = false;
   atomic_store_explicit(&aux->signal_high, 0, memory_order_relaxed);
   atomic_store_explicit(&aux->rewound, false, memory_order_relaxed);
   com->aux_destroy = npt_d3d12_fence_aux_destroy;
   /* Slot alloc + REGISTER happen post-Create (aux_init has no
    * InitialValue).  QI-routed wrappers stay unregistered and fall
    * through to the sync GetCompletedValue. */
   (void)dev; (void)host_id;
}

/* Record a requested Signal value and flag the fence non-monotonic on
 * any decrease.  Cross-TU: the queue's Signal override reaches the
 * fence aux through npt_d3d12_fence_note_signal below. */
static void
fence12_note_signal(struct npt_d3d12_fence_aux *aux, UINT64 value)
{
   if (!aux)
      return;
   uint64_t hi = atomic_load_explicit(&aux->signal_high, memory_order_relaxed);
   for (;;) {
      if (value < hi) {
         atomic_store_explicit(&aux->rewound, true, memory_order_release);
         return;
      }
      if (atomic_compare_exchange_weak_explicit(
             &aux->signal_high, &hi, value, memory_order_relaxed,
             memory_order_relaxed))
         return;
   }
}

static void
npt_d3d12_fence_aux_destroy(void *aux_raw)
{
   struct npt_d3d12_fence_aux *aux = aux_raw;
   /* Host-side unregister piggybacks on COM_RELEASE. */
   if (aux->base.fb_shmem && aux->base.com && aux->base.com->base.device) {
      npt_renderer_shmem_unref(aux->base.com->base.device->renderer,
                               aux->base.fb_shmem);
   }
   free(aux);
}

static void
npt_d3d12_fence_finalize_create(struct npt_device *dev, void *wrapper,
                                UINT64 initial_value)
{
   if (NPT_PERF(NO_FENCE_FEEDBACK))
      return;
   if (!dev || !wrapper)
      return;
   struct npt_com_base *com = wrapper;
   struct npt_d3d12_fence_aux *aux = com->aux;
   if (!aux || aux->base.registered)
      return;

   atomic_store_explicit(&aux->signal_high, initial_value,
                         memory_order_relaxed);

   uint32_t fb_offset = 0;
   bool fresh = false;
   struct npt_renderer_shmem *shmem =
      npt_device_alloc_feedback_slot(dev, NPT_FENCE_FEEDBACK_SLOT_SIZE,
                                     &fb_offset, &fresh);
   if (!shmem)
      return;

   /* Seed with InitialValue: the host poll only writes after a
    * Signal, so pre-Signal reads would otherwise see memset-zero. */
   struct npt_d3d11_fence_feedback_slot *slot =
      (struct npt_d3d11_fence_feedback_slot *)
      ((uint8_t *)shmem->mmap_ptr + fb_offset);
   atomic_store_explicit(&slot->completed_value, initial_value,
                         memory_order_relaxed);

   aux->base.fb_shmem = shmem;
   aux->base.fb_offset = fb_offset;

   if (fresh)
      npt_ring_force_roundtrip(dev->ring);

   if (npt_dispatch_feedback_register_fence(dev->ring, com->base.id,
                                            shmem->res_id, fb_offset,
                                            NPT_FENCE_FEEDBACK_API_D3D12))
      aux->base.registered = true;
}

/* NULL on wrappers that bypassed CreateFence; QI tier aliases resolve
 * to the primary's aux (and thus its registered feedback slot). */
static struct npt_d3d12_fence_aux *
fence12_aux(void *self)
{
   return npt_com_family_aux(self, npt_d3d12_fence_aux_destroy);
}

static struct npt_d3d11_fence_feedback_slot *
fence12_slot(struct npt_d3d12_fence_aux *aux)
{
   if (!aux || !aux->base.fb_shmem)
      return NULL;
   return (struct npt_d3d11_fence_feedback_slot *)
      ((uint8_t *)aux->base.fb_shmem->mmap_ptr + aux->base.fb_offset);
}

static UINT64 NPT_STDMETHODCALLTYPE
fence12_GetCompletedValue_override(void *self)
{
   struct npt_d3d12_fence_aux *aux = fence12_aux(self);
   struct npt_d3d11_fence_feedback_slot *slot = fence12_slot(aux);
   if (!slot || !aux->base.registered ||
       atomic_load_explicit(&aux->rewound, memory_order_acquire)) {
      /* Env opt-out, pool OOM, QI-routed wrapper, or a rewound fence
       * whose snapshot can read stale-high -- take the authoritative
       * sync read. */
      return npt_id3d12fence_default_GetCompletedValue(self);
   }
   /* Acquire pairs with the host's release store.  Monotonic fence:
    * the snapshot is a valid lower bound, matching the API contract. */
   return atomic_load_explicit(&slot->completed_value,
                               memory_order_acquire);
}

static HRESULT NPT_STDMETHODCALLTYPE
fence12_SetEventOnCompletion_override(void *self, UINT64 Value, HANDLE hEvent)
{
   struct npt_device *dev = npt_com_self_device(self);

   /* NULL-event form: the blocking wait.  The sync round-trip returns
    * once the host's SetEventOnCompletion(Value, NULL) call has waited
    * the fence out. */
   if (!hEvent) {
      return npt_call_ID3D12Fence_SetEventOnCompletion(
         npt_device_method_ring(dev), npt_com_self_id(self), Value, hEvent);
   }

   /* Fast path: the host publishes completed values to the feedback
    * slot; if the fence already reached Value, signal the event right
    * here and skip the wire arm entirely, which pacing waits hit
    * often.  A stale-low slot only means we
    * fall through to the normal arm -- conservative.  Skipped for a
    * rewound fence: its snapshot can read stale-high, which would latch
    * the event early with no way to un-signal. */
   struct npt_d3d12_fence_aux *aux = fence12_aux(self);
   struct npt_d3d11_fence_feedback_slot *slot = fence12_slot(aux);
   if (slot && aux->base.registered &&
       !atomic_load_explicit(&aux->rewound, memory_order_acquire) &&
       atomic_load_explicit(&slot->completed_value,
                            memory_order_acquire) >= Value) {
      SetEvent(hEvent);
      return NPT_S_OK;
   }

   /* Event form: arm first so the host's npt_win32_handle_replace sees
    * the proxy when it decodes hEvent; the registration itself then
    * rides async on the same ring -- ordered after the arm, and the
    * host fence worker fires the proxy whenever the value is reached,
    * so nothing needs a reply. */
   uint64_t token = npt_event_arm(dev, (void *)hEvent);
   if (!token)
      return NPT_E_FAIL;
   /* The wire HANDLE slot carries the single-use token; the host
    * resolves it to this arm's proxy. */
   npt_async_ID3D12Fence_SetEventOnCompletion(
      npt_device_method_ring(dev), npt_com_self_id(self), Value,
      (HANDLE)(uintptr_t)token);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
fence12_Signal_override(void *self, UINT64 Value)
{
   fence12_note_signal(fence12_aux(self), Value);
   return npt_id3d12fence_default_Signal(self, Value);
}

/* Cross-TU (npt_overrides_d3d12_queue.c): a CommandQueue::Signal
 * requests a fence value too, and can rewind it. */
void npt_d3d12_fence_note_signal(void *fence_wrapper, UINT64 value);

void
npt_d3d12_fence_note_signal(void *fence_wrapper, UINT64 value)
{
   fence12_note_signal(fence12_aux(fence_wrapper), value);
}

static HRESULT NPT_STDMETHODCALLTYPE
dev12_CreateFence_override(void *self, UINT64 InitialValue,
                           D3D12_FENCE_FLAGS Flags, const IID *riid,
                           void **ppFence)
{
   HRESULT hr = npt_id3d12device_default_CreateFence(
      self, InitialValue, Flags, riid, ppFence);
   if (NPT_SUCCEEDED(hr) && ppFence && *ppFence) {
      npt_d3d12_fence_finalize_create(npt_com_self_device(self),
                                      *ppFence, InitialValue);
   }
   return hr;
}

/*
 * Cross-TU entry for the D3D12 DDI (tritonQueue12.c): arm a GPU-true gate
 * on `fence_wrapper` (an ID3D12Fence npt wrapper -- the per-queue drain
 * fence) reaching `value`.  Wires up: KMD event-ring fence + token
 * (npt_event_gate_arm) and the host-side SetEventOnCompletion that
 * fires the proxy when the drain value completes on the GPU.  The caller
 * embeds *kmd_token_out in a VIOGPU_CMD_GATE DMA packet on the queue's
 * kernel context so dxgkrnl's monitored-fence packets wait for it.
 */
BOOL npt_d3d12_ecl_gate_arm(void *fence_wrapper, UINT64 value, UINT64 *kmd_token_out);

BOOL
npt_d3d12_ecl_gate_arm(void *fence_wrapper, UINT64 value, UINT64 *kmd_token_out)
{
   struct npt_device *dev = npt_com_self_device(fence_wrapper);
   if (!dev)
      return 0;
   uint64_t kmd = npt_event_gate_arm(dev, npt_com_self_id(fence_wrapper),
                                     value);
   if (!kmd)
      return 0;
   *kmd_token_out = kmd;
   return 1;
}

void
npt_overrides_d3d12_fence_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D12_FENCE(SetEventOnCompletion,
                                     fence12_SetEventOnCompletion_override);
   NPT_REGISTER_OVERRIDE_D3D12_FENCE(GetCompletedValue,
                                     fence12_GetCompletedValue_override);
   NPT_REGISTER_OVERRIDE_D3D12_FENCE(Signal, fence12_Signal_override);
   NPT_REGISTER_OVERRIDE_D3D12_DEVICE_ALL(CreateFence,
                                          dev12_CreateFence_override);
   npt_com_register_family(fence12_tiers,
                           sizeof(struct npt_d3d12_fence_aux),
                           npt_d3d12_fence_aux_init);
}
