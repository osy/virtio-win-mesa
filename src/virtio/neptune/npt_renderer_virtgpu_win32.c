/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Native Win32 virtgpu transport for Neptune.  Talks to viogpu3d.sys
 * over D3DKMT (no Wine, no unixlib).  Mirrors the D3DKMT plumbing the
 * sister Venus driver uses against the same KMD, adapted to the
 * Neptune renderer surface.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

/* WIN32_NO_STATUS keeps windows.h from defining a subset of STATUS_*
 * codes; we get the full set from ntstatus.h below. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS

#include <winnt.h>
#include <ntstatus.h>
#include <winternl.h>

#include <d3dkmthk.h>
#include <d3dukmdt.h>

#include "virtio/virtio-gpu/wddm_hw.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "npt_env.h"
#include "npt_renderer.h"

#define VIRTGPU_PCI_VENDOR_ID 0x1af4
#define VIRTGPU_PCI_DEVICE_ID 0x1050
#define VIRTGPU_WIN_DEVICE_ID "PCI\\VEN_1AF4&DEV_1050"

/* KMD SHMEM_GPU_BASE_VA: reserved in every process's GPU VA space.
 * D3DKMTSubmitCommand requires a nonzero Commands GPU VA, but the KMD
 * recovers the command body from the submission private data and never
 * dereferences this address. */
#define VIRTGPU_SHMEM_GPU_BASE_VA 0x700000000ull

struct npt_virtgpu_shmem {
   struct npt_renderer_shmem base;
   D3DKMT_HANDLE alloc;
   D3DKMT_HANDLE res_kmt;
   /* mmap_ptr came from the WDDM2 KMD's RES_INFO mapping rather than
    * D3DKMTLock; the KMD tears it down at allocation destroy. */
   bool kmd_mapped;
};

struct npt_virtgpu {
   struct npt_renderer base;

   /* Single global mutex guards the kernel-shared command buffer in
    * gpu->ctx; the kernel rotates pNew* pointers on every D3DKMTRender
    * so concurrent submits would race on cmd_buf / alloc_list. */
   CRITICAL_SECTION cs;

   HINSTANCE gdi32;
   struct {
      PFND3DKMT_QUERYADAPTERINFO queryAdapterInfo;
      PFND3DKMT_ESCAPE escape;
      PFND3DKMT_RENDER render;
      PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 signalSynchronizationObject2;
      PFND3DKMT_CREATECONTEXT createContext;
      PFND3DKMT_DESTROYCONTEXT destroyContext;
      PFND3DKMT_CREATEALLOCATION createAllocation;
      PFND3DKMT_DESTROYALLOCATION destroyAllocation;
      PFND3DKMT_LOCK lock;
      PFND3DKMT_UNLOCK unlock;
      PFND3DKMT_CREATEDEVICE createDevice;
      PFND3DKMT_DESTROYDEVICE destroyDevice;
      PFND3DKMT_OPENADAPTERFROMHDC openAdapterFromHdc;
      PFND3DKMT_CLOSEADAPTER closeAdapter;
      /* WDDM2 native submission set; optional (absence keeps the legacy
       * Render path available). */
      PFND3DKMT_CREATECONTEXTVIRTUAL createContextVirtual;
      PFND3DKMT_SUBMITCOMMAND submitCommand;
      PFND3DKMT_CREATESYNCHRONIZATIONOBJECT2 createSynchronizationObject2;
      PFND3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroySynchronizationObject;
      PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU signalSynchronizationObjectFromGpu;
      PFND3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU waitForSynchronizationObjectFromCpu;
   } cb;

   D3DKMT_HANDLE adapter;
   D3DKMT_HANDLE device;
   D3DKMT_HANDLE context;
   LUID luid;

   /* KMD runs in WDDM2 (GpuMmu) mode -- from VIOGPU_ADAPTERINFO.  Wire
    * selection for blob map/unmap depends on it, see virtgpu_map_blob_op. */
   bool wddm2;

   /* Kernel context is virtual (D3DKMTCreateContextVirtual) and commands
    * ride D3DKMTSubmitCommand private data instead of the Render command
    * buffer.  Set only when wddm2 and the full virtual bring-up (context +
    * monitored drain fence) succeeded. */
   bool virtual_ctx;
   D3DKMT_HANDLE drain_fence;
   /* Owned by drain_cs: the next drain value is minted and its
    * SignalFromGpu queued under it, see virtgpu_drain_monitored. */
   CRITICAL_SECTION drain_cs;
   UINT64 drain_value;
   /* Read-only CPU mapping of the drain fence value; already-satisfied
    * drains skip the kernel wait. */
   volatile const UINT64 *drain_fence_cpu_va;

   /* Kernel-mapped command buffer + alloc/patch lists, returned by
    * D3DKMTCreateContext and rotated by D3DKMTRender. */
   void *cmd_buf;
   UINT cmd_size;
   D3DDDI_ALLOCATIONLIST *alloc_list;
   UINT alloc_size;
   D3DDDI_PATCHLOCATIONLIST *patch_list;
   UINT patch_size;
};

static inline uint64_t
npt_align64(uint64_t v, uint64_t a)
{
   return (v + a - 1) & ~(a - 1);
}

static NTSTATUS
virtgpu_render(struct npt_virtgpu *gpu, UINT cmd_offset, UINT cmd_length,
               UINT alloc_count)
{
   D3DKMT_RENDER render = {
      .hContext = gpu->context,
      .CommandOffset = cmd_offset,
      .CommandLength = cmd_length,
      .AllocationCount = alloc_count,
      .PatchLocationCount = alloc_count,
   };

   NTSTATUS status = gpu->cb.render(&render);

   /* The kernel may rotate the buffers each call; pick up the new
    * pointers so the next render targets the current backing. */
   gpu->cmd_buf = render.pNewCommandBuffer;
   gpu->cmd_size = render.NewCommandBufferSize;
   gpu->alloc_list = render.pNewAllocationList;
   gpu->alloc_size = render.NewAllocationListSize;
   gpu->patch_list = render.pNewPatchLocationList;
   gpu->patch_size = render.NewPatchLocationListSize;

   return status;
}

static NTSTATUS
virtgpu_signal_event(struct npt_virtgpu *gpu, HANDLE event)
{
   D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 signal = {
      .hContext = gpu->context,
      .ObjectCount = 0,
      .BroadcastContextCount = 0,
      .Flags = { .EnqueueCpuEvent = TRUE },
      .CpuEventHandle = event,
   };
   return gpu->cb.signalSynchronizationObject2(&signal);
}

/* Monitored-fence drain for virtual contexts: a queued GPU-signal packet
 * retires only after all DMA previously queued on the context, so the
 * blocking CPU wait observes the same watermark the legacy EnqueueCpuEvent
 * drain did.
 *
 * Minting the value and queueing its signal happen under drain_cs:
 * dxgkrnl rejects a monitored-fence GPU signal below one already queued
 * on the context (STATUS_INVALID_PARAMETER), so concurrent drains must
 * queue in value order.  Only the queueing is serialized; the wait itself
 * runs unlocked. */
static bool
virtgpu_drain_monitored(struct npt_virtgpu *gpu)
{
   EnterCriticalSection(&gpu->drain_cs);
   const UINT64 value = ++gpu->drain_value;

   D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU signal = {
      .hContext = gpu->context,
      .ObjectCount = 1,
      .ObjectHandleArray = &gpu->drain_fence,
      .MonitoredFenceValueArray = &value,
   };
   NTSTATUS status = gpu->cb.signalSynchronizationObjectFromGpu(&signal);
   LeaveCriticalSection(&gpu->drain_cs);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu drain: SignalFromGpu failed 0x%lx (value=%llu)",
              status, (unsigned long long)value);
      return false;
   }

   if (gpu->drain_fence_cpu_va && *gpu->drain_fence_cpu_va >= value)
      return true;

   D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = gpu->device,
      .ObjectCount = 1,
      .ObjectHandleArray = &gpu->drain_fence,
      .FenceValueArray = &value,
   };
   status = gpu->cb.waitForSynchronizationObjectFromCpu(&wait);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu drain: WaitFromCpu failed 0x%lx", status);
      return false;
   }
   return true;
}

/* Submits a CPU-event signal on the current queue and blocks until the
 * KMD scheduler retires it, draining any pending work. */
static bool
virtgpu_drain(struct npt_virtgpu *gpu)
{
   if (gpu->virtual_ctx)
      return virtgpu_drain_monitored(gpu);

   HANDLE event = CreateEventA(NULL, TRUE, FALSE, NULL);
   if (!event)
      return false;

   NTSTATUS status = virtgpu_signal_event(gpu, event);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu drain: signal failed 0x%lx", status);
      CloseHandle(event);
      return false;
   }
   DWORD wr = WaitForSingleObject(event, INFINITE);
   CloseHandle(event);
   if (wr != WAIT_OBJECT_0) {
      npt_log("virtgpu drain: wait returned %lu", wr);
      return false;
   }
   return true;
}

static NTSTATUS
virtgpu_escape(struct npt_virtgpu *gpu, VIOGPU_ESCAPE *priv)
{
   D3DKMT_ESCAPE escape = {
      .hAdapter = gpu->adapter,
      .hDevice = gpu->device,
      .pPrivateDriverData = priv,
      .PrivateDriverDataSize = sizeof(*priv),
   };
   return gpu->cb.escape(&escape);
}

static NTSTATUS
virtgpu_get_caps(struct npt_virtgpu *gpu, uint32_t id, uint32_t version,
                 void *capset, uint32_t capset_size)
{
   VIOGPU_ESCAPE caps = {
      .Type = VIOGPU_GET_CAPS,
      .DataLength = sizeof(caps.Capset),
      .Capset = {
         .CapsetId = id,
         .Version = version,
         .Size = capset_size,
         .Capset = VioGpuUmPtr(capset),
      },
   };
   return virtgpu_escape(gpu, &caps);
}

static NTSTATUS
virtgpu_query_adapter_info(struct npt_virtgpu *gpu, void *priv,
                           UINT priv_size)
{
   D3DKMT_QUERYADAPTERINFO query = {
      .hAdapter = gpu->adapter,
      .Type = KMTQAITYPE_UMDRIVERPRIVATE,
      .pPrivateDriverData = priv,
      .PrivateDriverDataSize = priv_size,
   };
   return gpu->cb.queryAdapterInfo(&query);
}

/* Native WDDM2 submission: the command body travels by value in the DMA
 * packet's private driver data (GPU VAs are not CPU-mappable, and virtual
 * contexts have no kernel-mapped command buffer).  A wrong magic or an
 * oversized body makes the KMD execute the packet fence-only, silently
 * dropping the command -- hence the hard size guard. */
static NTSTATUS
virtgpu_submit_virtual_locked(struct npt_virtgpu *gpu, UINT type, UINT flags,
                              UINT ring_idx, const void *payload,
                              size_t payload_size)
{
   const size_t total = sizeof(VIOGPU_COMMAND_HDR) + payload_size;
   if (total > VIOGPU_DMA_PRIV_BODY_MAX) {
      npt_log("virtgpu: cmd %u too large for DMA private body (%zu > %u)",
              type, total, VIOGPU_DMA_PRIV_BODY_MAX);
      return STATUS_BUFFER_TOO_SMALL;
   }

   VIOGPU_DMA_PRIVATE priv;
   priv.cmdRaw = 0;
   priv.magic = VIOGPU_DMA_PRIV_MAGIC;
   priv.bodySize = (ULONG)total;

   VIOGPU_COMMAND_HDR *hdr = (VIOGPU_COMMAND_HDR *)priv.body;
   hdr->type = type;
   hdr->size = (UINT)payload_size;
   hdr->flags = flags;
   hdr->ring_idx = ring_idx;
   if (payload_size && payload)
      memcpy(hdr + 1, payload, payload_size);

   D3DKMT_SUBMITCOMMAND submit = {
      .Commands = VIRTGPU_SHMEM_GPU_BASE_VA,
      .CommandLength = (UINT)total,
      .BroadcastContextCount = 1,
      .BroadcastContext = { gpu->context },
      .pPrivateDriverData = &priv,
      .PrivateDriverDataSize = sizeof(priv),
   };
   return gpu->cb.submitCommand(&submit);
}

/* RAII-style: writes hdr + payload into the kernel command buffer
 * under gpu->cs, calls D3DKMTRender, and returns its status.  On a
 * virtual context there is no kernel command buffer; the same bytes ride
 * SubmitCommand private data instead.  Still serialized under gpu->cs to
 * preserve submit/map ordering. */
static NTSTATUS
virtgpu_send_cmd_locked(struct npt_virtgpu *gpu, UINT type, UINT flags,
                        UINT ring_idx, const void *payload, size_t payload_size,
                        UINT alloc_count)
{
   if (gpu->virtual_ctx) {
      /* Allocation references only exist on the legacy wire; the WDDM2
       * BY_ID wire never passes any. */
      if (alloc_count)
         return STATUS_INVALID_PARAMETER;
      return virtgpu_submit_virtual_locked(gpu, type, flags, ring_idx,
                                           payload, payload_size);
   }

   const size_t total = sizeof(VIOGPU_COMMAND_HDR) + payload_size;
   if (total > gpu->cmd_size) {
      npt_log("virtgpu: cmd %u too large for kernel buffer (%zu > %u)",
              type, total, gpu->cmd_size);
      return STATUS_BUFFER_TOO_SMALL;
   }

   VIOGPU_COMMAND_HDR *hdr = gpu->cmd_buf;
   hdr->type = type;
   hdr->size = (UINT)payload_size;
   hdr->flags = flags;
   hdr->ring_idx = ring_idx;

   if (payload_size && payload)
      memcpy(hdr + 1, payload, payload_size);

   return virtgpu_render(gpu, 0, (UINT)total, alloc_count);
}

/* Blob map/unmap wire depends on the KMD mode:
 * - WDDM2 (GpuMmu): MAP_BLOB_BY_ID / UNMAP_BLOB_BY_ID carry the res_id in
 *   the payload and reference no allocations -- allocation lists don't
 *   exist on virtual contexts (the KMD resolves the res_id through its
 *   open-time bindings).
 * - WDDM 1.3: keep the allocation-list wire.  BY_ID is NOT equivalent
 *   there: the allocation reference is what makes VidMm commit the blob's
 *   backing before the DMA executes, and switching 1.3 to BY_ID blacked
 *   the desktop (scanout armed, frames black -- A/B'd 2026-07-17). */
static NTSTATUS
virtgpu_map_blob_op(struct npt_virtgpu *gpu, D3DKMT_HANDLE alloc,
                    uint32_t res_id, UINT type)
{
   NTSTATUS status;
   EnterCriticalSection(&gpu->cs);
   if (gpu->wddm2) {
      if (type == VIOGPU_CMD_MAP_BLOB)
         type = VIOGPU_CMD_MAP_BLOB_BY_ID;
      else if (type == VIOGPU_CMD_UNMAP_BLOB)
         type = VIOGPU_CMD_UNMAP_BLOB_BY_ID;
      const ULONG payload = res_id;
      status =
         virtgpu_send_cmd_locked(gpu, type, 0, 0, &payload, sizeof(payload), 0);
   } else {
      gpu->alloc_list[0].hAllocation = alloc;
      gpu->patch_list[0].AllocationIndex = 0;
      const ULONG slot = 0;
      status =
         virtgpu_send_cmd_locked(gpu, type, 0, 0, &slot, sizeof(slot), 1);
   }
   LeaveCriticalSection(&gpu->cs);

   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: blob op 0x%x submit failed 0x%lx", type, status);
      return status;
   }
   if (!virtgpu_drain(gpu)) {
      npt_log("virtgpu: blob op 0x%x drain failed", type);
      return STATUS_UNSUCCESSFUL;
   }
   return STATUS_SUCCESS;
}

static NTSTATUS
virtgpu_lock(struct npt_virtgpu *gpu, D3DKMT_HANDLE alloc, void **out_ptr)
{
   D3DKMT_LOCK lock = {
      .hDevice = gpu->device,
      .hAllocation = alloc,
      .Flags = { .LockEntire = 1 },
   };
   NTSTATUS status = gpu->cb.lock(&lock);
   if (!NT_SUCCESS(status))
      return status;
   *out_ptr = lock.pData;
   return STATUS_SUCCESS;
}

static NTSTATUS
virtgpu_unlock(struct npt_virtgpu *gpu, D3DKMT_HANDLE alloc)
{
   D3DKMT_UNLOCK unlock = {
      .hDevice = gpu->device,
      .NumAllocations = 1,
      .phAllocations = &alloc,
   };
   return gpu->cb.unlock(&unlock);
}

static NTSTATUS
virtgpu_resource_destroy_blob(struct npt_virtgpu *gpu,
                             D3DKMT_HANDLE alloc, D3DKMT_HANDLE res_kmt);

/* Create a virtio blob via D3DKMTCreateAllocation, look up the host
 * res_id via the RES_INFO escape, and (for mappable blobs) issue the
 * MAP_BLOB ring command so the kernel maps the host backing into the
 * guest address space.  Lock follows separately.  Any failure after the
 * allocation is created destroys it before returning. */
static NTSTATUS
virtgpu_resource_create_blob(struct npt_virtgpu *gpu, uint32_t blob_mem,
                             uint32_t blob_flags, size_t blob_size,
                             uint64_t blob_id, uint32_t *out_res_id,
                             D3DKMT_HANDLE *out_alloc,
                             D3DKMT_HANDLE *out_res_kmt,
                             void **out_user_va)
{
   if (out_user_va)
      *out_user_va = NULL;
   blob_size = (size_t)npt_align64(blob_size, 4096);

   VIOGPU_CREATE_ALLOCATION_EXCHANGE alloc_priv = {
      .Type = VIOGPU_RESOURCE_TYPE_BLOB,
      .OptionsBlob = {
         .blob_mem = blob_mem,
         .blob_flags = blob_flags,
         .blob_id = blob_id,
      },
      .Size = blob_size,
   };
   VIOGPU_CREATE_RESOURCE_EXCHANGE res_priv = { 0 };
   D3DDDI_ALLOCATIONINFO alloc_info = {
      .pPrivateDriverData = &alloc_priv,
      .PrivateDriverDataSize = sizeof(alloc_priv),
   };

   const bool is_shareable = !!(blob_flags & VIOGPU_BLOB_FLAG_USE_SHAREABLE);
   const bool is_mappable = !!(blob_flags & VIOGPU_BLOB_FLAG_USE_MAPPABLE);

   if (!virtgpu_drain(gpu))
      return STATUS_UNSUCCESSFUL;

   D3DKMT_CREATEALLOCATION alloc = {
      .hDevice = gpu->device,
      .pPrivateDriverData = &res_priv,
      .PrivateDriverDataSize = sizeof(res_priv),
      .NumAllocations = 1,
      .pAllocationInfo = &alloc_info,
      .Flags = {
         .CreateResource = 1,
         .CreateShared = is_shareable,
      },
   };
   NTSTATUS status = gpu->cb.createAllocation(&alloc);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: D3DKMTCreateAllocation failed 0x%lx", status);
      return status;
   }

   *out_alloc = alloc_info.hAllocation;
   *out_res_kmt = alloc.hResource;

   if (!virtgpu_drain(gpu)) {
      status = STATUS_UNSUCCESSFUL;
      goto fail_destroy;
   }

   /* On a WDDM2 (GpuMmu) KMD the RES_INFO escape returns UserVa: the
    * KMD owns the shmem placement and maps BAR+offset into this process
    * (VidMm locks hand out system staging pages there, never the BAR
    * window).  On WDDM 1.3 UserVa stays 0 and D3DKMTLock keeps working. */
   VIOGPU_ESCAPE res_info = {
      .Type = VIOGPU_RES_INFO,
      .DataLength = sizeof(res_info.ResourceInfo),
      .ResourceInfo = { .ResHandle = *out_alloc },
   };
   status = virtgpu_escape(gpu, &res_info);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: RES_INFO escape failed 0x%lx", status);
      goto fail_destroy;
   }
   if (out_user_va)
      *out_user_va = (void *)(uintptr_t)res_info.ResourceInfo.UserVa;
   if (!res_info.ResourceInfo.IsBlob || !res_info.ResourceInfo.IsCreated) {
      npt_log("virtgpu: RES_INFO reports blob not created");
      status = STATUS_INVALID_PARAMETER;
      goto fail_destroy;
   }
   *out_res_id = res_info.ResourceInfo.Id;

   if (is_mappable) {
      status = virtgpu_map_blob_op(gpu, *out_alloc, *out_res_id, VIOGPU_CMD_MAP_BLOB);
      if (!NT_SUCCESS(status))
         goto fail_destroy;
   }
   return STATUS_SUCCESS;

fail_destroy:
   virtgpu_resource_destroy_blob(gpu, *out_alloc, *out_res_kmt);
   *out_alloc = 0;
   *out_res_kmt = 0;
   return status;
}

static NTSTATUS
virtgpu_resource_destroy_blob(struct npt_virtgpu *gpu,
                              D3DKMT_HANDLE alloc, D3DKMT_HANDLE res_kmt)
{
   if (!virtgpu_drain(gpu))
      return STATUS_UNSUCCESSFUL;

   D3DKMT_DESTROYALLOCATION destroy = {
      .hDevice = gpu->device,
      .hResource = res_kmt,
      .AllocationCount = res_kmt == 0 ? 1 : 0,
      .phAllocationList = res_kmt == 0 ? &alloc : NULL,
   };
   NTSTATUS status = gpu->cb.destroyAllocation(&destroy);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: D3DKMTDestroyAllocation failed 0x%lx", status);
      return status;
   }
   return virtgpu_drain(gpu) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* Attach an existing VM-global virtio resource to this transport
 * context via a VIOGPU_RESOURCE_TYPE_IMPORT allocation, so the host
 * worker receives the resource fd (proxy attach-forwarding) before a
 * SHARED_OPEN_RES names it. */
static bool
npt_vgw32_import_res(struct npt_renderer *r, uint32_t res_id, uint64_t size,
                     uint32_t *out_alloc, uint32_t *out_res_kmt)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;

   VIOGPU_CREATE_ALLOCATION_EXCHANGE alloc_priv = {
      .Type = VIOGPU_RESOURCE_TYPE_IMPORT,
      .OptionsImport = {
         .res_id = res_id,
      },
      .Size = npt_align64(size ? size : 4096, 4096),
   };
   VIOGPU_CREATE_RESOURCE_EXCHANGE res_priv = { 0 };
   D3DDDI_ALLOCATIONINFO alloc_info = {
      .pPrivateDriverData = &alloc_priv,
      .PrivateDriverDataSize = sizeof(alloc_priv),
   };

   if (!virtgpu_drain(gpu))
      return false;

   D3DKMT_CREATEALLOCATION alloc = {
      .hDevice = gpu->device,
      .pPrivateDriverData = &res_priv,
      .PrivateDriverDataSize = sizeof(res_priv),
      .NumAllocations = 1,
      .pAllocationInfo = &alloc_info,
      .Flags = {
         .CreateResource = 1,
      },
   };
   NTSTATUS status = gpu->cb.createAllocation(&alloc);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: import_res res_id=%u failed 0x%lx", res_id, status);
      return false;
   }

   *out_alloc = alloc_info.hAllocation;
   *out_res_kmt = alloc.hResource;

   /* Flush so the KMD's CTX_ATTACH_RESOURCE reaches the host before
    * the caller's ring command names the resource. */
   if (!virtgpu_drain(gpu))
      npt_log("virtgpu: import_res res_id=%u post-drain failed", res_id);

   npt_log("virtgpu: import_res res_id=%u alloc=0x%x", res_id,
           alloc_info.hAllocation);
   return true;
}

static void
npt_vgw32_release_import_res(struct npt_renderer *r, uint32_t alloc,
                             uint32_t res_kmt)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;
   if (!alloc && !res_kmt)
      return;
   virtgpu_resource_destroy_blob(gpu, (D3DKMT_HANDLE)alloc,
                                 (D3DKMT_HANDLE)res_kmt);
}

static struct npt_renderer_shmem *
npt_vgw32_shmem_create(struct npt_renderer *r, size_t size)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;

   const size_t alloc_size = (size_t)npt_align64(size, 4096);

   D3DKMT_HANDLE alloc = 0, res_kmt = 0;
   uint32_t res_id = 0;
   void *kmd_va = NULL;
   NTSTATUS status = virtgpu_resource_create_blob(
      gpu, VIOGPU_BLOB_MEM_HOST3D,
      VIOGPU_BLOB_FLAG_USE_MAPPABLE,
      alloc_size, 0, &res_id, &alloc, &res_kmt, &kmd_va);
   if (!NT_SUCCESS(status))
      return NULL;

   /* WDDM2 KMD hands us its own mapping; on 1.3 fall back to the
    * classic VidMm lock. */
   void *ptr = kmd_va;
   if (!ptr) {
      status = virtgpu_lock(gpu, alloc, &ptr);
      if (!NT_SUCCESS(status)) {
         npt_log("virtgpu: lock failed 0x%lx", status);
         virtgpu_map_blob_op(gpu, alloc, res_id, VIOGPU_CMD_UNMAP_BLOB);
         virtgpu_resource_destroy_blob(gpu, alloc, res_kmt);
         return NULL;
      }
   }

   struct npt_virtgpu_shmem *s = npt_alloc(sizeof(*s));
   if (!s) {
      if (!kmd_va)
         virtgpu_unlock(gpu, alloc);
      virtgpu_map_blob_op(gpu, alloc, res_id, VIOGPU_CMD_UNMAP_BLOB);
      virtgpu_resource_destroy_blob(gpu, alloc, res_kmt);
      return NULL;
   }
   npt_refcount_init(&s->base.refcount);
   s->base.res_id = res_id;
   s->base.size = alloc_size;
   s->base.mmap_ptr = ptr;
   s->alloc = alloc;
   s->res_kmt = res_kmt;
   s->kmd_mapped = kmd_va != NULL;
   return &s->base;
}

static void
npt_vgw32_shmem_destroy(struct npt_renderer *r, struct npt_renderer_shmem *_s)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;
   struct npt_virtgpu_shmem *s = (struct npt_virtgpu_shmem *)_s;
   if (!s)
      return;

   if (!s->kmd_mapped)
      virtgpu_unlock(gpu, s->alloc);
   virtgpu_map_blob_op(gpu, s->alloc, s->base.res_id, VIOGPU_CMD_UNMAP_BLOB);
   virtgpu_resource_destroy_blob(gpu, s->alloc, s->res_kmt);
   free(s);
}

static bool
npt_vgw32_submit_cmd(struct npt_renderer *r, const void *data, size_t size)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;

   EnterCriticalSection(&gpu->cs);
   NTSTATUS status =
      virtgpu_send_cmd_locked(gpu, VIOGPU_CMD_SUBMIT,
                              VIOGPU_EXECBUF_RING_IDX, 0, data, size, 0);
   LeaveCriticalSection(&gpu->cs);

   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu submit_cmd: render failed 0x%lx", status);
      return false;
   }
   return true;
}

static bool
npt_vgw32_submit_cmd_sync(struct npt_renderer *r, const void *data,
                          size_t size)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;

   if (!npt_vgw32_submit_cmd(r, data, size))
      return false;

   return virtgpu_drain(gpu);
}

/* Arm a Win32 event for the next GPU retirement on the requested ring: create an
 * auto-reset event and hand it to the KMD via VIOGPU_SUBMIT_PRESENT_FENCE, which
 * signals it at GPU completion.  npt_event's waiter blocks on it (wait_one) and
 * then SetEvents the caller's app event.
 *
 * The renderer interface return is int (sync_file-FD shape on Linux).  Windows
 * handles are 32-bit-significant (MSDN 32/64-bit interop), so the value
 * survives; callers treat a negative int as failure, so a handle with bit 31 set
 * would be misread as an error. */
static int
npt_vgw32_submit_present_fence(struct npt_renderer *r, uint32_t ring_idx)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;
   HANDLE hWait = CreateEventW(NULL, /*bManualReset*/ FALSE, /*bInitial*/ FALSE, NULL);
   if (!hWait)
      return -1;
   VIOGPU_ESCAPE esc = {
      .Type = VIOGPU_SUBMIT_PRESENT_FENCE,
      .DataLength = sizeof(esc.PresentFence),
      .PresentFence = { .EventUM = VioGpuUmHandle(hWait), .RingIdx = ring_idx },
   };
   NTSTATUS status = virtgpu_escape(gpu, &esc);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: SUBMIT_PRESENT_FENCE failed 0x%lx ring=%u", status, ring_idx);
      CloseHandle(hWait);
      return -1;
   }
   return (int)(intptr_t)hWait;
}

/* Monitored-fence gate arm: same empty fenced SUBMIT_3D on an event ring,
 * but retirement fires the returned KMD token (no UM event) so a
 * VIOGPU_CMD_GATE DMA packet on a D3D12 queue's kernel context can hold
 * its completion until the GPU truly drained. */
static uint64_t
npt_vgw32_arm_gate_fence(struct npt_renderer *r, uint32_t ring_idx)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;
   VIOGPU_ESCAPE esc = {
      .Type = VIOGPU_ARM_GATE,
      .DataLength = sizeof(esc.GateArm),
      .GateArm = { .RingIdx = ring_idx, .Token = 0 },
   };
   NTSTATUS status = virtgpu_escape(gpu, &esc);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: ARM_GATE failed 0x%lx ring=%u", status, ring_idx);
      return 0;
   }
   return esc.GateArm.Token;
}

static void
npt_vgw32_destroy(struct npt_renderer *r)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;

   if (gpu->context && gpu->cb.destroyContext) {
      D3DKMT_DESTROYCONTEXT destroy = { .hContext = gpu->context };
      gpu->cb.destroyContext(&destroy);
   }
   /* After the context (no queued signal packet may outlive it), before
    * the device (device-scoped object). */
   if (gpu->drain_fence && gpu->cb.destroySynchronizationObject) {
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {
         .hSyncObject = gpu->drain_fence,
      };
      gpu->cb.destroySynchronizationObject(&destroy);
   }
   if (gpu->device && gpu->cb.destroyDevice) {
      D3DKMT_DESTROYDEVICE destroy = { .hDevice = gpu->device };
      gpu->cb.destroyDevice(&destroy);
   }
   if (gpu->adapter && gpu->cb.closeAdapter) {
      D3DKMT_CLOSEADAPTER close = { .hAdapter = gpu->adapter };
      gpu->cb.closeAdapter(&close);
   }
   if (gpu->gdi32)
      FreeLibrary(gpu->gdi32);

   DeleteCriticalSection(&gpu->drain_cs);
   DeleteCriticalSection(&gpu->cs);
   free(gpu);
}

static bool
virtgpu_load_d3dkmt(struct npt_virtgpu *gpu)
{
   gpu->gdi32 = LoadLibraryA("GDI32.dll");
   if (!gpu->gdi32) {
      npt_log("virtgpu: failed to load GDI32.dll");
      return false;
   }

#define GETPROC(field, fn) \
   gpu->cb.field = (void *)GetProcAddress(gpu->gdi32, "D3DKMT" #fn)

   GETPROC(queryAdapterInfo, QueryAdapterInfo);
   GETPROC(escape, Escape);
   GETPROC(render, Render);
   GETPROC(signalSynchronizationObject2, SignalSynchronizationObject2);
   GETPROC(createContext, CreateContext);
   GETPROC(destroyContext, DestroyContext);
   GETPROC(createAllocation, CreateAllocation);
   GETPROC(destroyAllocation, DestroyAllocation);
   GETPROC(lock, Lock);
   GETPROC(unlock, Unlock);
   GETPROC(createDevice, CreateDevice);
   GETPROC(destroyDevice, DestroyDevice);
   GETPROC(openAdapterFromHdc, OpenAdapterFromHdc);
   GETPROC(closeAdapter, CloseAdapter);
   GETPROC(createContextVirtual, CreateContextVirtual);
   GETPROC(submitCommand, SubmitCommand);
   GETPROC(createSynchronizationObject2, CreateSynchronizationObject2);
   GETPROC(destroySynchronizationObject, DestroySynchronizationObject);
   GETPROC(signalSynchronizationObjectFromGpu, SignalSynchronizationObjectFromGpu);
   GETPROC(waitForSynchronizationObjectFromCpu, WaitForSynchronizationObjectFromCpu);

#undef GETPROC

   if (!gpu->cb.queryAdapterInfo || !gpu->cb.escape || !gpu->cb.render ||
       !gpu->cb.signalSynchronizationObject2 || !gpu->cb.createContext ||
       !gpu->cb.createAllocation || !gpu->cb.destroyAllocation ||
       !gpu->cb.lock || !gpu->cb.unlock || !gpu->cb.createDevice ||
       !gpu->cb.openAdapterFromHdc || !gpu->cb.closeAdapter) {
      npt_log("virtgpu: GDI32 missing required D3DKMT entry points");
      return false;
   }
   return true;
}

static bool
virtgpu_find_adapter(struct npt_virtgpu *gpu)
{
   DISPLAY_DEVICEA dev = { .cb = sizeof(dev) };

   for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, (DISPLAY_DEVICE *)&dev, 0);
        i++) {
      if (_strnicmp(dev.DeviceID, VIRTGPU_WIN_DEVICE_ID,
                    strlen(VIRTGPU_WIN_DEVICE_ID)) != 0)
         continue;

      HDC hdc = CreateDCA(NULL, dev.DeviceName, NULL, NULL);
      if (!hdc) {
         npt_log("virtgpu: CreateDC failed for %s", dev.DeviceName);
         continue;
      }
      D3DKMT_OPENADAPTERFROMHDC open = { .hDc = hdc };
      NTSTATUS status = gpu->cb.openAdapterFromHdc(&open);
      DeleteDC(hdc);
      if (!NT_SUCCESS(status)) {
         npt_log("virtgpu: OpenAdapterFromHdc failed 0x%lx for %s",
                 status, dev.DeviceName);
         continue;
      }
      gpu->adapter = open.hAdapter;
      gpu->luid = open.AdapterLuid;
      npt_debug("virtgpu: opened adapter %s (LUID %lx-%lx)",
                dev.DeviceName, open.AdapterLuid.HighPart,
                open.AdapterLuid.LowPart);
      return true;
   }
   npt_log("virtgpu: no PCI device matching %s", VIRTGPU_WIN_DEVICE_ID);
   return false;
}

static bool
virtgpu_create_device(struct npt_virtgpu *gpu)
{
   D3DKMT_CREATEDEVICE create = { .hAdapter = gpu->adapter };
   NTSTATUS status = gpu->cb.createDevice(&create);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: D3DKMTCreateDevice failed 0x%lx", status);
      return false;
   }
   gpu->device = create.hDevice;
   return true;
}

static bool
virtgpu_create_context(struct npt_virtgpu *gpu)
{
   D3DKMT_CREATECONTEXT create = {
      .hDevice = gpu->device,
      .ClientHint = D3DKMT_CLIENTHINT_VULKAN,
   };
   NTSTATUS status = gpu->cb.createContext(&create);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: D3DKMTCreateContext failed 0x%lx", status);
      return false;
   }
   gpu->context = create.hContext;
   gpu->cmd_buf = create.pCommandBuffer;
   gpu->cmd_size = create.CommandBufferSize;
   gpu->alloc_list = create.pAllocationList;
   gpu->alloc_size = create.AllocationListSize;
   gpu->patch_list = create.pPatchLocationList;
   gpu->patch_size = create.PatchLocationListSize;
   return true;
}

/* Virtual-addressing context + monitored drain fence.  No create-time
 * private data: the KMD reads only the kernel-side VirtualAddressing flag
 * and negotiates the per-submit private-data size itself.  All-or-nothing:
 * a partial bring-up is torn down so the caller can fall back wholesale to
 * the legacy context. */
static bool
virtgpu_create_context_virtual(struct npt_virtgpu *gpu)
{
   if (!gpu->cb.createContextVirtual || !gpu->cb.submitCommand ||
       !gpu->cb.createSynchronizationObject2 ||
       !gpu->cb.destroySynchronizationObject ||
       !gpu->cb.signalSynchronizationObjectFromGpu ||
       !gpu->cb.waitForSynchronizationObjectFromCpu ||
       !gpu->cb.destroyContext)
      return false;

   D3DKMT_CREATECONTEXTVIRTUAL create = {
      .hDevice = gpu->device,
      .ClientHint = D3DKMT_CLIENTHINT_VULKAN,
   };
   NTSTATUS status = gpu->cb.createContextVirtual(&create);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: D3DKMTCreateContextVirtual failed 0x%lx", status);
      return false;
   }
   gpu->context = create.hContext;

   D3DKMT_CREATESYNCHRONIZATIONOBJECT2 fence = {
      .hDevice = gpu->device,
      .Info = { .Type = D3DDDI_MONITORED_FENCE },
   };
   status = gpu->cb.createSynchronizationObject2(&fence);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: monitored drain fence create failed 0x%lx", status);
      D3DKMT_DESTROYCONTEXT destroy = { .hContext = gpu->context };
      gpu->cb.destroyContext(&destroy);
      gpu->context = 0;
      return false;
   }
   gpu->drain_fence = fence.hSyncObject;
   gpu->drain_fence_cpu_va =
      (volatile const UINT64 *)fence.Info.MonitoredFence.FenceValueCPUVirtualAddress;
   return true;
}

static struct npt_adapter_probe g_adapter_probe;

/* One-shot adapter/host capability probe on a throwaway D3DKMT device;
 * no kernel context is created and everything is torn down before
 * returning.  Runs the same bring-up sequence as the real transport so
 * the two can never disagree about the KMD mode. */
static void
npt_adapter_probe_impl(void)
{
   struct npt_virtgpu *gpu = npt_alloc(sizeof(*gpu));
   if (!gpu)
      return;
   InitializeCriticalSection(&gpu->cs);
   InitializeCriticalSection(&gpu->drain_cs);

   if (!virtgpu_load_d3dkmt(gpu))
      goto out;
   if (!virtgpu_find_adapter(gpu))
      goto out;
   if (!virtgpu_create_device(gpu))
      goto out;

   VIOGPU_ADAPTERINFO info = { 0 };
   if (!NT_SUCCESS(virtgpu_query_adapter_info(gpu, &info, sizeof(info))))
      goto out;
   if (info.IamVioGPU != VIOGPU_IAM || !info.Flags.Supports3d ||
       !info.Flags.HasShmem ||
       !(info.SupportedCapsetIDs & (1ull << NPT_CAPSET_ID)))
      goto out;
   g_adapter_probe.viogpu = true;
   g_adapter_probe.wddm2 = info.Flags.Wddm2 != 0;

   struct npt_capset capset = { 0 };
   if (!NT_SUCCESS(virtgpu_get_caps(gpu, NPT_CAPSET_ID, 0, &capset,
                                    sizeof(capset))))
      goto out;
   if (capset.wire_format_version != NPT_PROTOCOL_WIRE_VERSION)
      goto out;
   g_adapter_probe.host_ok = true;
   g_adapter_probe.caps_flags = capset.caps_flags;

out:
   npt_vgw32_destroy(&gpu->base);
}

const struct npt_adapter_probe *
npt_adapter_probe(void)
{
   static _Atomic int state;
   NPT_CALL_ONCE(state, npt_adapter_probe_impl());
   return &g_adapter_probe;
}

struct npt_renderer *
npt_renderer_create_virtgpu(void)
{
   struct npt_virtgpu *gpu = npt_alloc(sizeof(*gpu));
   if (!gpu)
      return NULL;
   InitializeCriticalSection(&gpu->cs);
   InitializeCriticalSection(&gpu->drain_cs);

   if (!virtgpu_load_d3dkmt(gpu))
      goto fail;
   if (!virtgpu_find_adapter(gpu))
      goto fail;
   if (!virtgpu_create_device(gpu))
      goto fail;

   VIOGPU_ADAPTERINFO info = { 0 };
   NTSTATUS status = virtgpu_query_adapter_info(gpu, &info, sizeof(info));
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: QueryAdapterInfo failed 0x%lx", status);
      goto fail;
   }
   if (info.IamVioGPU != VIOGPU_IAM || !info.Flags.Supports3d ||
       !info.Flags.HasShmem) {
      npt_log("virtgpu: KMD reports no 3D/shmem support (iam=0x%llx flags=0x%x)",
              (unsigned long long)info.IamVioGPU,
              *(unsigned *)&info.Flags);
      goto fail;
   }
   if (!(info.SupportedCapsetIDs & (1ull << NPT_CAPSET_ID))) {
      npt_log("virtgpu: KMD does not advertise Neptune capset");
      goto fail;
   }
   gpu->wddm2 = info.Flags.Wddm2 != 0;
   npt_log("virtgpu: KMD mode %s", gpu->wddm2 ? "WDDM2 (GpuMmu)" : "WDDM 1.3");

   struct npt_capset capset = { 0 };
   status = virtgpu_get_caps(gpu, NPT_CAPSET_ID, 0, &capset, sizeof(capset));
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: GET_CAPS for Neptune failed 0x%lx", status);
      goto fail;
   }
   if (capset.wire_format_version != NPT_PROTOCOL_WIRE_VERSION) {
      npt_log("virtgpu: wire format mismatch host=0x%08x guest=0x%08x",
              capset.wire_format_version,
              (unsigned)NPT_PROTOCOL_WIRE_VERSION);
      goto fail;
   }

   VIOGPU_ESCAPE ctx_init = {
      .Type = VIOGPU_CTX_INIT,
      .DataLength = sizeof(ctx_init.CtxInit),
      .CtxInit = {
         .CapsetID = NPT_CAPSET_ID,
         .NumRings = 64,
         .DebugName = "neptune-win32",
      },
   };
   status = virtgpu_escape(gpu, &ctx_init);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: CTX_INIT failed 0x%lx", status);
      goto fail;
   }

   npt_env_init();
   if (gpu->wddm2 && virtgpu_create_context_virtual(gpu)) {
      gpu->virtual_ctx = true;
      npt_log("virtgpu: kernel context virtual (native WDDM2 submit)");
   } else if (!virtgpu_create_context(gpu))
      goto fail;

   gpu->base.info.wire_format_version = capset.wire_format_version;
   gpu->base.info.caps_flags = capset.caps_flags;
   gpu->base.info.max_timeline_count = 64;
   /* The KMD targets this context by id when another device's flip
    * present submits this transport's WSI_PRESENT bytes. */
   gpu->base.info.virtio_ctx_id = ctx_init.CtxInit.CtxId;

   gpu->base.ops.destroy = npt_vgw32_destroy;
   gpu->base.ops.submit_cmd = npt_vgw32_submit_cmd;
   gpu->base.ops.submit_cmd_sync = npt_vgw32_submit_cmd_sync;
   gpu->base.ops.submit_present_fence = npt_vgw32_submit_present_fence;
   gpu->base.ops.arm_gate_fence = npt_vgw32_arm_gate_fence;
   gpu->base.ops.import_res = npt_vgw32_import_res;
   gpu->base.ops.release_import_res = npt_vgw32_release_import_res;
   /* create_host_blob and wsi_* are wired alongside the DXGI swapchain
    * Present path; the renderer helpers NULL-check them. */

   gpu->base.shmem_ops.create = npt_vgw32_shmem_create;
   gpu->base.shmem_ops.destroy = npt_vgw32_shmem_destroy;

   npt_log("virtgpu: renderer ready (wire=0x%08x, caps=0x%08x, max_timeline=%u)",
           capset.wire_format_version, capset.caps_flags,
           gpu->base.info.max_timeline_count);
   return &gpu->base;

fail:
   npt_vgw32_destroy(&gpu->base);
   return NULL;
}

struct npt_renderer *
npt_renderer_create_vtest(void)
{
   return NULL;
}
