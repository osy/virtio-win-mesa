/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Host capability snapshot for the adapter-scope GetCaps handlers.
 *
 * The DDI caps the runtime asks for at OpenAdapter time are answered
 * from the host backend's own CheckFeatureSupport, taken once per
 * adapter on an inner Neptune device that the first CreateDevice then
 * adopts.  Every field the handlers report goes through one of three
 * policies, which is the only hand-maintained part:
 *
 *   HOST   the host's answer, forwarded (the default);
 *   FIXED  Triton's own answer, final -- the host value is irrelevant
 *          (KMD-owned facts, feature-level labels);
 *   TODO   Triton's own answer because the guest side is missing work
 *          (an unimplemented DDI, an unvalidated path).  When the host
 *          is ahead of the reported value the caps ledger flags it.
 *
 * Each field is logged once per process as
 *   Triton: caps[d3d12] OPTIONS.ResourceBindingTier host=3 -> 3 [host]
 * so `Triton: caps[` in a debug capture is the live version of the
 * two-sided caps audit, and every `[TODO` line is a work item.
 *
 * A field the host did not answer (feature rejected, or no host device)
 * keeps the handler's zero fill.
 */

#ifndef TRITON_HOST_CAPS_H
#define TRITON_HOST_CAPS_H

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- D3D12 ---------- */

enum {
    TRITON_HC12_OPTIONS      = 1u << 0,
    TRITON_HC12_ARCH1        = 1u << 1,
    TRITON_HC12_GPUVA        = 1u << 2,
    TRITON_HC12_SHADER_MODEL = 1u << 3,
    TRITON_HC12_OPTIONS1     = 1u << 4,
    TRITON_HC12_OPTIONS2     = 1u << 5,
    TRITON_HC12_OPTIONS3     = 1u << 6,
    TRITON_HC12_OPTIONS4     = 1u << 7,
    TRITON_HC12_OPTIONS5     = 1u << 8,
    TRITON_HC12_OPTIONS6     = 1u << 9,
    TRITON_HC12_OPTIONS7     = 1u << 10,
    TRITON_HC12_OPTIONS9     = 1u << 11,
    TRITON_HC12_OPTIONS11    = 1u << 12,
};

/* Feature ids past the SDK the driver is built against are still asked
 * for: the host's ceiling is not the header's. */
#define TRITON_HC12_FEATURE_OPTIONS9  37
#define TRITON_HC12_FEATURE_OPTIONS11 40

struct triton_hc12_options9 {
    BOOL MeshShaderPipelineStatsSupported;
    BOOL MeshShaderSupportsFullRangeRenderTargetArrayIndex;
    BOOL AtomicInt64OnTypedResourceSupported;
    BOOL AtomicInt64OnGroupSharedSupported;
    BOOL DerivativesInMeshAndAmplificationShadersSupported;
    UINT WaveMMATier;
};

struct triton_hc12_options11 {
    BOOL AtomicInt64OnDescriptorHeapResourceSupported;
};

struct triton_host_caps12 {
    /* A host device answered; `have` says which features returned S_OK.
     * A feature the host rejects (E_INVALIDARG = no case in its switch)
     * is absent, so the handler keeps its zero fill for that field. */
    BOOL probed;
    UINT have;
    D3D12_FEATURE_DATA_D3D12_OPTIONS            options;
    D3D12_FEATURE_DATA_ARCHITECTURE1            arch1;
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT gpuva;
    D3D12_FEATURE_DATA_SHADER_MODEL             shaderModel;
    D3D12_FEATURE_DATA_D3D12_OPTIONS1           options1;
    D3D12_FEATURE_DATA_D3D12_OPTIONS2           options2;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3           options3;
    D3D12_FEATURE_DATA_D3D12_OPTIONS4           options4;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5           options5;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6           options6;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7           options7;
    struct triton_hc12_options9                 options9;
    struct triton_hc12_options11                options11;
};

/* Stand up an inner Neptune ID3D12Device and fill \p out from it.  On
 * success *ppDev holds the device for the caller to adopt or release.
 * Returns FALSE (out->probed == FALSE) when the host device cannot be
 * created; the handlers then report zero fill. */
BOOL tritonHostCaps12Snapshot(struct triton_host_caps12 *out,
                              ID3D12Device **ppDev);

/* ---------- D3D11 ---------- */

enum {
    TRITON_HC11_THREADING  = 1u << 0,
    TRITON_HC11_DOUBLES    = 1u << 1,
    TRITON_HC11_D3D10X     = 1u << 2,
    TRITON_HC11_OPTIONS    = 1u << 3,
    TRITON_HC11_ARCH       = 1u << 4,
    TRITON_HC11_MINPREC    = 1u << 5,
    TRITON_HC11_OPTIONS1   = 1u << 6,
    TRITON_HC11_OPTIONS2   = 1u << 7,
    TRITON_HC11_OPTIONS3   = 1u << 8,
    TRITON_HC11_GPUVA      = 1u << 9,
    TRITON_HC11_SHADERCACHE = 1u << 10,
};

struct triton_host_caps11 {
    BOOL probed;
    UINT have;
    D3D_FEATURE_LEVEL featureLevel;
    D3D11_FEATURE_DATA_THREADING                threading;
    D3D11_FEATURE_DATA_DOUBLES                  doubles;
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS d3d10x;
    D3D11_FEATURE_DATA_D3D11_OPTIONS            options;
    D3D11_FEATURE_DATA_ARCHITECTURE_INFO        arch;
    D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT minPrec;
    D3D11_FEATURE_DATA_D3D11_OPTIONS1           options1;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2           options2;
    D3D11_FEATURE_DATA_D3D11_OPTIONS3           options3;
    D3D11_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT gpuva;
    D3D11_FEATURE_DATA_SHADER_CACHE             shaderCache;
};

/* D3D11 twin: the inner device is created at the highest feature level
 * the host grants (10_0..11_1 offered) and returned with its immediate
 * context for the first CreateDevice to adopt. */
BOOL tritonHostCaps11Snapshot(struct triton_host_caps11 *out,
                              ID3D11Device **ppDev,
                              ID3D11DeviceContext **ppCtx);

/* ---------- policy ledger ---------- */

enum triton_cap_policy {
    TRITON_CAP_POLICY_HOST,
    TRITON_CAP_POLICY_FIXED,
    TRITON_CAP_POLICY_TODO,
};

void tritonHostCapsNote(const char *api, const char *name, BOOL hostKnown,
                        UINT64 hostValue, UINT64 reported,
                        enum triton_cap_policy policy, const char *reason);

/* A cap the host reports but this DDI revision has no field for. */
void tritonHostCapsNoteUnreachable(const char *api, const char *name,
                                   BOOL hostKnown, UINT64 hostValue,
                                   const char *reason);

#define TRITON_CAP_NOTE_ONCE(api, name, known, hostval, reported, policy, reason) \
    do {                                                                       \
        static LONG _tr_cap_once;                                              \
        if (!InterlockedExchange(&_tr_cap_once, 1))                            \
            tritonHostCapsNote(api, name, known, (UINT64)(hostval),            \
                               (UINT64)(reported), policy, reason);            \
    } while (0)

/* lvalue = host value when the host answered; otherwise untouched
 * (the handler's zero fill). */
#define TRITON_CAP_HOST(api, name, lvalue, known, hostval)                     \
    do {                                                                       \
        BOOL _k = (known);                                                     \
        if (_k)                                                                \
            (lvalue) = (hostval);                                              \
        TRITON_CAP_NOTE_ONCE(api, name, _k, _k ? (UINT64)(hostval) : 0,        \
                             (lvalue), TRITON_CAP_POLICY_HOST, NULL);          \
    } while (0)

/* lvalue = value; the host's answer is recorded in the ledger only. */
#define TRITON_CAP_FIXED(api, name, lvalue, known, hostval, value, reason)     \
    do {                                                                       \
        BOOL _k = (known);                                                     \
        (lvalue) = (value);                                                    \
        TRITON_CAP_NOTE_ONCE(api, name, _k, _k ? (UINT64)(hostval) : 0,        \
                             (lvalue), TRITON_CAP_POLICY_FIXED, reason);       \
    } while (0)

#define TRITON_CAP_TODO(api, name, lvalue, known, hostval, value, reason)      \
    do {                                                                       \
        BOOL _k = (known);                                                     \
        (lvalue) = (value);                                                    \
        TRITON_CAP_NOTE_ONCE(api, name, _k, _k ? (UINT64)(hostval) : 0,        \
                             (lvalue), TRITON_CAP_POLICY_TODO, reason);        \
    } while (0)

#define TRITON_CAP_UNREACHABLE(api, name, known, hostval, reason)              \
    do {                                                                       \
        static LONG _tr_cap_once;                                              \
        BOOL _k = (known);                                                     \
        if (!InterlockedExchange(&_tr_cap_once, 1))                            \
            tritonHostCapsNoteUnreachable(api, name, _k,                       \
                                          _k ? (UINT64)(hostval) : 0, reason); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* TRITON_HOST_CAPS_H */
