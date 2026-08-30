/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Host capability snapshot (see tritonHostCaps.h): stands up the inner
 * Neptune device the adapter-scope GetCaps handlers need, asks it the
 * feature structs the handlers translate from, and prints the caps
 * ledger.
 *
 * Each CheckFeatureSupport is a synchronous round trip (it writes a
 * guest buffer), taken once per adapter; the elapsed time is logged so
 * the cost stays measured.
 */

#include "triton12.h"
#include "triton_log.h"
#include "tritonHostCaps.h"

#include <stdio.h>

/* Statically linked from npt_entry_d3d12.c / npt_entry_d3d11.c. */
HRESULT
npt_d3d12_create_device_internal(IUnknown *pAdapter,
                                 D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                 REFIID riid,
                                 void **ppDevice);
HRESULT
npt_d3d11_create_device_internal(IDXGIAdapter *pAdapter,
                                 D3D_DRIVER_TYPE DriverType,
                                 HMODULE Software,
                                 UINT Flags,
                                 const D3D_FEATURE_LEVEL *pFeatureLevels,
                                 UINT FeatureLevels,
                                 UINT SDKVersion,
                                 ID3D11Device **ppDevice,
                                 D3D_FEATURE_LEVEL *pFeatureLevel,
                                 ID3D11DeviceContext **ppImmediateContext);

static double
hc_elapsed_ms(const LARGE_INTEGER *t0)
{
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    return (double)(t1.QuadPart - t0->QuadPart) * 1000.0 / (double)f.QuadPart;
}

/* ---------- D3D12 ---------- */

BOOL
tritonHostCaps12Snapshot(struct triton_host_caps12 *s, ID3D12Device **ppDev)
{
    memset(s, 0, sizeof(*s));
    *ppDev = NULL;

    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    ID3D12Device *dev = NULL;
    HRESULT hr = npt_d3d12_create_device_internal(
        NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    if (FAILED(hr) || !dev) {
        TR_LOG("caps[d3d12]: inner device create failed 0x%08lx; "
               "reporting zero fill", (unsigned long)hr);
        return FALSE;
    }
    const double createMs = hc_elapsed_ms(&t0);

    UINT asked = 0;
#define HC12_QUERY(bit, feature, field)                                      \
    do {                                                                     \
        asked++;                                                             \
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(                      \
                dev, (D3D12_FEATURE)(feature), &s->field, sizeof(s->field))))\
            s->have |= (bit);                                                \
    } while (0)

    HC12_QUERY(TRITON_HC12_OPTIONS,  D3D12_FEATURE_D3D12_OPTIONS,  options);
    HC12_QUERY(TRITON_HC12_ARCH1,    D3D12_FEATURE_ARCHITECTURE1,  arch1);
    HC12_QUERY(TRITON_HC12_GPUVA,    D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, gpuva);
    HC12_QUERY(TRITON_HC12_OPTIONS1, D3D12_FEATURE_D3D12_OPTIONS1, options1);
    HC12_QUERY(TRITON_HC12_OPTIONS2, D3D12_FEATURE_D3D12_OPTIONS2, options2);
    HC12_QUERY(TRITON_HC12_OPTIONS3, D3D12_FEATURE_D3D12_OPTIONS3, options3);
    HC12_QUERY(TRITON_HC12_OPTIONS4, D3D12_FEATURE_D3D12_OPTIONS4, options4);
    HC12_QUERY(TRITON_HC12_OPTIONS5, D3D12_FEATURE_D3D12_OPTIONS5, options5);
    HC12_QUERY(TRITON_HC12_OPTIONS6, D3D12_FEATURE_D3D12_OPTIONS6, options6);
    HC12_QUERY(TRITON_HC12_OPTIONS7, D3D12_FEATURE_D3D12_OPTIONS7, options7);
    HC12_QUERY(TRITON_HC12_OPTIONS9, TRITON_HC12_FEATURE_OPTIONS9, options9);
    HC12_QUERY(TRITON_HC12_OPTIONS11, TRITON_HC12_FEATURE_OPTIONS11, options11);
#undef HC12_QUERY

    /* SHADER_MODEL is "ask high, get told what you may have": an
     * unknown model is E_INVALIDARG, so walk down from the newest. */
    {
        static const UINT asks[] = { 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
                                     0x63, 0x62, 0x61, 0x60, 0x51 };
        for (UINT i = 0; i < sizeof(asks) / sizeof(asks[0]); i++) {
            asked++;
            s->shaderModel.HighestShaderModel = (D3D_SHADER_MODEL)asks[i];
            if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(
                    dev, D3D12_FEATURE_SHADER_MODEL, &s->shaderModel,
                    sizeof(s->shaderModel)))) {
                s->have |= TRITON_HC12_SHADER_MODEL;
                break;
            }
        }
        if (!(s->have & TRITON_HC12_SHADER_MODEL))
            s->shaderModel.HighestShaderModel = (D3D_SHADER_MODEL)0;
    }

    s->probed = TRUE;
    *ppDev = dev;
    TR_LOG("caps[d3d12]: snapshot have=0x%04x (%u queries) device %.1f ms, "
           "queries %.1f ms", s->have, asked, createMs,
           hc_elapsed_ms(&t0) - createMs);
    return TRUE;
}

/* ---------- D3D11 ---------- */

BOOL
tritonHostCaps11Snapshot(struct triton_host_caps11 *s, ID3D11Device **ppDev,
                         ID3D11DeviceContext **ppCtx)
{
    memset(s, 0, sizeof(*s));
    *ppDev = NULL;
    *ppCtx = NULL;

    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
    HRESULT hr = npt_d3d11_create_device_internal(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels,
        sizeof(levels) / sizeof(levels[0]), D3D11_SDK_VERSION,
        &dev, &fl, &ctx);
    if (FAILED(hr) || !dev || !ctx) {
        TR_LOG("caps[d3d11]: inner device create failed 0x%08lx; "
               "reporting zero fill", (unsigned long)hr);
        if (dev) ID3D11Device_Release(dev);
        if (ctx) ID3D11DeviceContext_Release(ctx);
        return FALSE;
    }
    const double createMs = hc_elapsed_ms(&t0);
    s->featureLevel = fl;

    UINT asked = 0;
#define HC11_QUERY(bit, feature, field)                                      \
    do {                                                                     \
        asked++;                                                             \
        if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(                      \
                dev, (D3D11_FEATURE)(feature), &s->field, sizeof(s->field))))\
            s->have |= (bit);                                                \
    } while (0)

    HC11_QUERY(TRITON_HC11_THREADING, D3D11_FEATURE_THREADING, threading);
    HC11_QUERY(TRITON_HC11_DOUBLES,   D3D11_FEATURE_DOUBLES, doubles);
    HC11_QUERY(TRITON_HC11_D3D10X,    D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, d3d10x);
    HC11_QUERY(TRITON_HC11_OPTIONS,   D3D11_FEATURE_D3D11_OPTIONS, options);
    HC11_QUERY(TRITON_HC11_ARCH,      D3D11_FEATURE_ARCHITECTURE_INFO, arch);
    HC11_QUERY(TRITON_HC11_MINPREC,   D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, minPrec);
    HC11_QUERY(TRITON_HC11_OPTIONS1,  D3D11_FEATURE_D3D11_OPTIONS1, options1);
    HC11_QUERY(TRITON_HC11_OPTIONS2,  D3D11_FEATURE_D3D11_OPTIONS2, options2);
    HC11_QUERY(TRITON_HC11_OPTIONS3,  D3D11_FEATURE_D3D11_OPTIONS3, options3);
    HC11_QUERY(TRITON_HC11_GPUVA,     D3D11_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, gpuva);
    HC11_QUERY(TRITON_HC11_SHADERCACHE, D3D11_FEATURE_SHADER_CACHE, shaderCache);
#undef HC11_QUERY

    s->probed = TRUE;
    *ppDev = dev;
    *ppCtx = ctx;
    TR_LOG("caps[d3d11]: snapshot have=0x%04x (%u queries) fl=0x%x device "
           "%.1f ms, queries %.1f ms", s->have, asked, (unsigned)fl,
           createMs, hc_elapsed_ms(&t0) - createMs);
    return TRUE;
}

/* ---------- ledger ---------- */

void
tritonHostCapsNote(const char *api, const char *name, BOOL hostKnown,
                   UINT64 hostValue, UINT64 reported,
                   enum triton_cap_policy policy, const char *reason)
{
    char host[24];
    if (hostKnown)
        _snprintf_s(host, sizeof(host), _TRUNCATE, "%llu",
                    (unsigned long long)hostValue);
    else
        _snprintf_s(host, sizeof(host), _TRUNCATE, "?");

    switch (policy) {
    case TRITON_CAP_POLICY_HOST:
        TR_LOG("caps[%s] %s host=%s -> %llu [host]", api, name, host,
               (unsigned long long)reported);
        break;
    case TRITON_CAP_POLICY_FIXED:
        TR_LOG("caps[%s] %s host=%s -> %llu [fixed: %s]", api, name, host,
               (unsigned long long)reported, reason ? reason : "");
        break;
    case TRITON_CAP_POLICY_TODO:
        if (hostKnown && hostValue > reported)
            TR_LOG("caps[%s] %s host=%s -> %llu [TODO host ahead: %s]", api,
                   name, host, (unsigned long long)reported,
                   reason ? reason : "");
        else
            TR_LOG("caps[%s] %s host=%s -> %llu [todo, host not ahead: %s]",
                   api, name, host, (unsigned long long)reported,
                   reason ? reason : "");
        break;
    }
}

void
tritonHostCapsNoteUnreachable(const char *api, const char *name,
                              BOOL hostKnown, UINT64 hostValue,
                              const char *reason)
{
    if (!hostKnown || !hostValue)
        return;
    TR_LOG("caps[%s] %s host=%llu -> (no field at this DDI revision) "
           "[TODO unreachable: %s]", api, name,
           (unsigned long long)hostValue, reason ? reason : "");
}
