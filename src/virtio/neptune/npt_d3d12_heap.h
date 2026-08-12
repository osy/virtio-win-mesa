/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D12 persistently-mapped heap path: CPU-visible
 * heaps (UPLOAD / READBACK / CPU-accessible CUSTOM) are backed by a
 * dedicated guest SHM blob that the host imports as the heap's
 * VkDeviceMemory via ID3D12Device13::OpenExistingHeapFromAddress1
 * (VK_EXT_external_memory_host).  The app's Map() then returns a
 * pointer straight into the guest mmap -- zero wire traffic -- while
 * the GPU reads/writes the same pages.
 */

#ifndef NPT_D3D12_HEAP_H
#define NPT_D3D12_HEAP_H

#include "npt_com.h"
#include "npt_renderer.h"

/* Dedicated blob (not pool-sub-allocated: import is at heap
 * granularity and the blob must outlive the host import). */
struct npt_d3d12_shmem_heap {
   struct npt_renderer_shmem *shmem;
   uint64_t size;
};

static inline void *
npt_d3d12_shmem_heap_ptr(const struct npt_d3d12_shmem_heap *h)
{
   return (h && h->shmem) ? h->shmem->mmap_ptr : NULL;
}

/* Aux attached to every ID3D12Heap{,1} wrapper.  shmem_heap is NULL
 * for ordinary (host-allocated) heaps. */
struct npt_d3d12_heap_aux {
   struct npt_com_base *com;
   struct npt_d3d12_shmem_heap *shmem_heap;  /* owned */
   D3D12_HEAP_DESC app_desc;                 /* GetDesc answers guest-side */
   bool has_app_desc;
};

/* NULL on wrappers outside the heap family (e.g. QI-minted ids that
 * bypassed the ctor table). */
struct npt_d3d12_heap_aux *
npt_d3d12_heap_aux_cast(void *heap_wrapper);

/*
 * Allocate a page/64KiB-aligned SHM blob of `size` bytes, import it on
 * the host as an ID3D12Heap (sync CREATE_HEAP_FROM_SHMEM), and return
 * a heap wrapper (pub_ref == 1, caller-owned) with its aux carrying
 * {shmem_heap, app_desc}.  `riid` must be ID3D12Heap or ID3D12Heap1.
 * NULL on any failure (caller falls back to the generated default /
 * sync-map path).
 */
void *
npt_d3d12_heap_create_shmem_backed(void *device_wrapper,
                                   const GUID *riid,
                                   uint64_t size,
                                   const D3D12_HEAP_DESC *app_desc);

/*
 * Record shmem-heap backing on a freshly created resource wrapper
 * (implemented in npt_overrides_d3d12_resource.c where the resource
 * aux lives).  Takes its own ref on heap_wrapper (released in the
 * resource aux destroy).  buffer_width > 0 enables the zero-wire Map
 * fast path (buffers only); pass 0 to only pin the heap + record the
 * app heap properties.  False when either wrapper lacks its aux.
 */
bool
npt_d3d12_resource_bind_shmem_heap(void *resource_wrapper,
                                   void *heap_wrapper,
                                   uint64_t heap_offset,
                                   uint64_t buffer_width,
                                   const D3D12_HEAP_PROPERTIES *app_props,
                                   D3D12_HEAP_FLAGS app_flags);

void npt_d3d12_heap_overrides_init(void);

#endif /* NPT_D3D12_HEAP_H */
