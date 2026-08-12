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
#include "npt_renderer.h"

#define VIRTGPU_PCI_VENDOR_ID 0x1af4
#define VIRTGPU_PCI_DEVICE_ID 0x1050
#define VIRTGPU_WIN_DEVICE_ID "PCI\\VEN_1AF4&DEV_1050"

struct npt_virtgpu_shmem {
   struct npt_renderer_shmem base;
   D3DKMT_HANDLE alloc;
   D3DKMT_HANDLE res_kmt;
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
   } cb;

   D3DKMT_HANDLE adapter;
   D3DKMT_HANDLE device;
   D3DKMT_HANDLE context;
   LUID luid;

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

/* Submits a CPU-event signal on the current queue and blocks until the
 * KMD scheduler retires it, draining any pending work. */
static bool
virtgpu_drain(struct npt_virtgpu *gpu)
{
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

/* RAII-style: writes hdr + payload into the kernel command buffer
 * under gpu->cs, calls D3DKMTRender, and returns its status. */
static NTSTATUS
virtgpu_send_cmd_locked(struct npt_virtgpu *gpu, UINT type, UINT flags,
                        UINT ring_idx, const void *payload, size_t payload_size,
                        UINT alloc_count)
{
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

/* MAP_BLOB / UNMAP_BLOB take a single allocation reference in slot 0,
 * indexed via a single-ULONG payload. */
static NTSTATUS
virtgpu_map_blob_op(struct npt_virtgpu *gpu, D3DKMT_HANDLE alloc, UINT type)
{
   EnterCriticalSection(&gpu->cs);
   gpu->alloc_list[0].hAllocation = alloc;
   gpu->patch_list[0].AllocationIndex = 0;

   const ULONG slot = 0;
   NTSTATUS status =
      virtgpu_send_cmd_locked(gpu, type, 0, 0, &slot, sizeof(slot), 1);
   LeaveCriticalSection(&gpu->cs);

   if (!NT_SUCCESS(status))
      return status;
   return virtgpu_drain(gpu) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
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
                             D3DKMT_HANDLE *out_res_kmt)
{
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
   if (!res_info.ResourceInfo.IsBlob || !res_info.ResourceInfo.IsCreated) {
      npt_log("virtgpu: RES_INFO reports blob not created");
      status = STATUS_INVALID_PARAMETER;
      goto fail_destroy;
   }
   *out_res_id = res_info.ResourceInfo.Id;

   if (is_mappable) {
      status = virtgpu_map_blob_op(gpu, *out_alloc, VIOGPU_CMD_MAP_BLOB);
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
   NTSTATUS status = virtgpu_resource_create_blob(
      gpu, VIOGPU_BLOB_MEM_HOST3D,
      VIOGPU_BLOB_FLAG_USE_MAPPABLE,
      alloc_size, 0, &res_id, &alloc, &res_kmt);
   if (!NT_SUCCESS(status))
      return NULL;

   void *ptr = NULL;
   status = virtgpu_lock(gpu, alloc, &ptr);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: lock failed 0x%lx", status);
      virtgpu_map_blob_op(gpu, alloc, VIOGPU_CMD_UNMAP_BLOB);
      virtgpu_resource_destroy_blob(gpu, alloc, res_kmt);
      return NULL;
   }

   struct npt_virtgpu_shmem *s = npt_alloc(sizeof(*s));
   if (!s) {
      virtgpu_unlock(gpu, alloc);
      virtgpu_map_blob_op(gpu, alloc, VIOGPU_CMD_UNMAP_BLOB);
      virtgpu_resource_destroy_blob(gpu, alloc, res_kmt);
      return NULL;
   }
   npt_refcount_init(&s->base.refcount);
   s->base.res_id = res_id;
   s->base.size = alloc_size;
   s->base.mmap_ptr = ptr;
   s->alloc = alloc;
   s->res_kmt = res_kmt;
   return &s->base;
}

static void
npt_vgw32_shmem_destroy(struct npt_renderer *r, struct npt_renderer_shmem *_s)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;
   struct npt_virtgpu_shmem *s = (struct npt_virtgpu_shmem *)_s;
   if (!s)
      return;

   virtgpu_unlock(gpu, s->alloc);
   virtgpu_map_blob_op(gpu, s->alloc, VIOGPU_CMD_UNMAP_BLOB);
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

struct npt_renderer *
npt_renderer_create_virtgpu(void)
{
   struct npt_virtgpu *gpu = npt_alloc(sizeof(*gpu));
   if (!gpu)
      return NULL;
   InitializeCriticalSection(&gpu->cs);

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

   if (!virtgpu_create_context(gpu))
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
