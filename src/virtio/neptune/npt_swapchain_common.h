/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * State and machinery shared by the D3D11 and D3D12 guest-fabricated
 * swapchains: the exportable present pool, the WSI worker and X
 * IDLE_NOTIFY drain threads, the CPU-runahead latency ring, the pool
 * reuse gate, and every DXGI method that only touches this state.
 *
 * Each swapchain embeds struct npt_guest_swapchain_common as its first
 * member, so the aux pointer of either converts to the common state and
 * the shared vtbl methods serve both.
 */

#ifndef NPT_SWAPCHAIN_COMMON_H
#define NPT_SWAPCHAIN_COMMON_H

#include "npt_com.h"

#include "neptune-protocol/npt_protocol_directx_types.h"

#if defined(__WINE__)

#include <stdatomic.h>
#include <windows.h>

#include "neptune-protocol/npt_protocol_client_idxgioutput.h"

/* DXGI constants the protocol types header doesn't carry. */
#define NPT_DXGI_PRESENT_TEST            0x00000001u
#define NPT_DXGI_STATUS_OCCLUDED         ((HRESULT)0x087A0001)

/* Present-pool depth: enough for one on-screen + one queued + one
 * being written.  Must fit the unixlib's image table. */
#define NPT_SC_PRESENT_IMAGES 3u

/* CPU-runahead cap: ring of dup'd GPU-done fence fds; a Present that
 * finds the ring full first waits on the oldest.  Mirrors DXGI's
 * MaximumFrameLatency semantics (default 3). */
#define NPT_SC_PRESENT_LATENCY 3u

/* Async WSI worker queue.  poll(fence) + xcb_present run on a per-
 * swapchain worker so the app's Present thread never blocks on the
 * host GPU; the latency ring above is the pacing bound. */
#define NPT_SC_WSI_QUEUE_SIZE 8u

/* Reuse-gate budget: how long Present waits for the compositor to
 * release the next pool image before proceeding anyway (a minimized
 * window never idles). */
#define NPT_SC_REUSE_WAIT_MS 100u

/* DXGI's DXGI_MAX_SWAP_CHAIN_BUFFERS; also caps MaximumFrameLatency
 * and the frame-latency waitable semaphore. */
#define NPT_SC_MAX_SWAP_CHAIN_BUFFERS 16u

/* Consecutive never-idled presents before Present reports
 * DXGI_STATUS_OCCLUDED. */
#define NPT_SC_OCCLUDED_STREAK 4u

struct npt_gsc_present_entry {
   uint32_t image_index;
   int      wait_fence_fd;   /* -1 = unfenced; ownership -> worker */
   uint64_t token;           /* event token to release after present */
};

struct npt_guest_swapchain_common {
   struct npt_com_base *com;

   /* Log prefix naming the owning swapchain type. */
   const char *tag;

   /* Guest-authoritative description. */
   DXGI_SWAP_CHAIN_DESC1 desc;
   DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc;
   HWND hwnd;
   BOOL fullscreen;
   UINT max_frame_latency;
   DXGI_RGBA background;
   UINT present_count;

   /* Frame-latency waitable object (non-NULL only when the app created
    * the swapchain with FRAME_LATENCY_WAITABLE_OBJECT).  Released once
    * per Present so the app's wait paces it to max_frame_latency. */
   HANDLE frame_latency_sem;

   /* Present pool: exportable host textures bound to guest blobs whose
    * dmabufs feed the X11 WSI. */
   bool wsi_initialized;
   bool wsi_failed;
   void *present_tex[NPT_SC_PRESENT_IMAGES];
   uint64_t present_tex_id[NPT_SC_PRESENT_IMAGES];
   /* ++ at present, -- per X IDLE_NOTIFY (drain thread). */
   _Atomic uint32_t image_inflight[NPT_SC_PRESENT_IMAGES];
   /* Auto-reset event the drain thread signals on each release so the
    * Present reuse-gate blocks instead of polling. */
   HANDLE image_release_evt;
   /* Consecutive presents whose image never idled (hidden/minimized
    * window): drives the DXGI_STATUS_OCCLUDED return so the app
    * throttles instead of spinning. */
   uint32_t occluded_streak;

   /* Single lock over Present/Resize/GetBuffer. */
   mtx_t present_mutex;

   /* Latency ring (dup'd GPU-done fds). */
   int latency_fences[NPT_SC_PRESENT_LATENCY];
   uint32_t latency_count;

   /* Async WSI worker. */
   mtx_t wsi_mutex;
   cnd_t wsi_cond;
   struct npt_gsc_present_entry wsi_queue[NPT_SC_WSI_QUEUE_SIZE];
   uint32_t wsi_head, wsi_tail;
   _Atomic int wsi_thread_exit;
   bool wsi_thread_started;
   HANDLE wsi_thread;

   /* X IDLE_NOTIFY release-count drain. */
   HANDLE drain_thread;
   _Atomic int drain_exit;
   bool drain_started;

   /* IDXGIOutput bookkeeping (guest-fab outputs never reach the wire). */
   mtx_t output_lock;
   IDXGIOutput *fullscreen_target;
   IDXGIOutput *containing_output;
};

static inline struct npt_guest_swapchain_common *
npt_gsc(void *self)
{
   return ((struct npt_com_base *)self)->aux;
}

static inline const struct npt_idxgioutput_client_vtbl *
npt_gsc_output_vtbl(IDXGIOutput *o)
{
   return (const void *)((struct npt_com_base *)o)->lpVtbl;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

void
npt_gsc_init(struct npt_guest_swapchain_common *c, struct npt_com_base *com,
             const DXGI_SWAP_CHAIN_DESC1 *desc1,
             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
             HWND hwnd, const char *tag);

void
npt_gsc_fini(struct npt_guest_swapchain_common *c);

/* ------------------------------------------------------------------ */
/* held-wrapper helpers                                                */
/* ------------------------------------------------------------------ */

void
npt_gsc_release_held(struct npt_device *dev, void **slot, uint64_t *id);

HRESULT
npt_gsc_qi_held(void *wrapper, const IID *riid, void **out);

/* ------------------------------------------------------------------ */
/* WSI lifecycle                                                       */
/* ------------------------------------------------------------------ */

bool
npt_gsc_wsi_backend_available(struct npt_guest_swapchain_common *c);

bool
npt_gsc_wsi_bringup(struct npt_guest_swapchain_common *c, uint32_t fmt);

void
npt_gsc_wsi_quiesce(struct npt_guest_swapchain_common *c, bool shutting_down);

void
npt_gsc_release_pool(struct npt_guest_swapchain_common *c);

void
npt_gsc_wsi_push(struct npt_guest_swapchain_common *c, uint32_t image_index,
                 int wait_fence_fd, uint64_t token);

/* ------------------------------------------------------------------ */
/* present helpers                                                     */
/* ------------------------------------------------------------------ */

uint64_t
npt_gsc_make_token(void);

void
npt_gsc_frame_latency_release(struct npt_guest_swapchain_common *c);

void
npt_gsc_backpressure_wait(struct npt_guest_swapchain_common *c);

void
npt_gsc_backpressure_record(struct npt_guest_swapchain_common *c,
                            int fence_fd);

void
npt_gsc_reuse_gate(struct npt_guest_swapchain_common *c, uint32_t image);

void
npt_gsc_query_window_size(struct npt_guest_swapchain_common *c,
                          UINT *width, UINT *height);

void
npt_gsc_window_resize(HWND hwnd, uint32_t w, uint32_t h);

/* ------------------------------------------------------------------ */
/* outputs                                                             */
/* ------------------------------------------------------------------ */

void
npt_gsc_assign_output(struct npt_guest_swapchain_common *c,
                      IDXGIOutput **slot, IDXGIOutput *new_val);

IDXGIOutput *
npt_gsc_load_output_addref(struct npt_guest_swapchain_common *c,
                           IDXGIOutput **slot);

void
npt_gsc_release_outputs(struct npt_guest_swapchain_common *c);

/* ------------------------------------------------------------------ */
/* shared vtbl methods                                                 */
/* ------------------------------------------------------------------ */

HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_QueryInterface(void *self, REFIID riid, void **ppvObject);
ULONG NPT_STDMETHODCALLTYPE
npt_gsc_AddRef(void *self);
ULONG NPT_STDMETHODCALLTYPE
npt_gsc_Release(void *self);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetPrivateData(void *self, const GUID *Name, UINT DataSize,
                       const void *pData);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetPrivateDataInterface(void *self, const GUID *Name,
                                const IUnknown *pUnknown);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetPrivateData(void *self, const GUID *Name, UINT *pDataSize,
                       void *pData);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetDesc(void *self, DXGI_SWAP_CHAIN_DESC *pDesc);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetDesc1(void *self, DXGI_SWAP_CHAIN_DESC1 *pDesc);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFullscreenDesc(void *self, DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetHwnd(void *self, HWND *pHwnd);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetCoreWindow(void *self, const IID *refiid, void **ppUnk);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFullscreenState(void *self, BOOL *pFullscreen,
                           IDXGIOutput **ppTarget);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetRestrictToOutput(void *self, IDXGIOutput **ppRestrictToOutput);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetFrameStatistics(void *self, DXGI_FRAME_STATISTICS *pStats);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetLastPresentCount(void *self, UINT *pLastPresentCount);
BOOL NPT_STDMETHODCALLTYPE
npt_gsc_IsTemporaryMonoSupported(void *self);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetBackgroundColor(void *self, const DXGI_RGBA *pColor);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetBackgroundColor(void *self, DXGI_RGBA *pColor);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetRotation(void *self, DXGI_MODE_ROTATION Rotation);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetRotation(void *self, DXGI_MODE_ROTATION *pRotation);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetSourceSize(void *self, UINT Width, UINT Height);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetSourceSize(void *self, UINT *pWidth, UINT *pHeight);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetMaximumFrameLatency(void *self, UINT MaxLatency);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetMaximumFrameLatency(void *self, UINT *pMaxLatency);
HANDLE NPT_STDMETHODCALLTYPE
npt_gsc_GetFrameLatencyWaitableObject(void *self);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetMatrixTransform(void *self, const DXGI_MATRIX_3X2_F *pMatrix);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_GetMatrixTransform(void *self, DXGI_MATRIX_3X2_F *pMatrix);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_CheckColorSpaceSupport(void *self, DXGI_COLOR_SPACE_TYPE ColorSpace,
                               UINT *pColorSpaceSupport);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetColorSpace1(void *self, DXGI_COLOR_SPACE_TYPE ColorSpace);
HRESULT NPT_STDMETHODCALLTYPE
npt_gsc_SetHDRMetaData(void *self, DXGI_HDR_METADATA_TYPE Type, UINT Size,
                       void *pMetaData);

#endif /* __WINE__ */

#endif /* NPT_SWAPCHAIN_COMMON_H */
