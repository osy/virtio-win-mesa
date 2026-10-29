/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXBC container builders and the bounds-checked signature parser shared by
 * the D3D10/11 and D3D12 UMDs (tritonDxbc.c).
 */

#ifndef TRITON_DXBC_H_INCLUDED
#define TRITON_DXBC_H_INCLUDED

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wrap a DDI token stream plus its DDI signature arrays in a DXBC
 * container.  entryStride is the DDI signature-entry size. */
void *tritonBuildDxbc(const UINT *pTokens, SIZE_T cbTokens,
                      const void *pInEntries,    UINT cInSigs,
                      const void *pOutEntries,   UINT cOutSigs,
                      const void *pPatchEntries, UINT cPatchSigs,
                      UINT entryStride,
                      SIZE_T *pcbOut);

/* Synthetic ISGN-only container describing exactly the given elements. */
void *tritonBuildInputSigDxbc(const char *const *pNames,
                              const UINT *pSemanticIndices,
                              const UINT *pRegisters,
                              UINT cEntries, SIZE_T *pcbOut);

/* Whether a pixel shader program (SM5 token stream) declares the vCoverage
 * input, and a copy of its tritonBuildDxbc container with every vCoverage
 * read replaced by `value`: NULL when there is none, or one sits inside a
 * relative index, where no immediate can go; see tritonDxbc.c for the
 * target-independent-rasterization use. */
BOOL tritonDxbcReadsInputCoverage(const UINT *pTokens, SIZE_T cbTokens);
void *tritonDxbcFixInputCoverage(const void *pDxbc, SIZE_T cbDxbc, UINT value,
                                 SIZE_T *pcbOut);

/* VS input-signature reconciler (see tritonDxbc.c for rationale). */
void *tritonReconcileVsInputSig(const void *pDxbc, SIZE_T cbDxbc,
                                const unsigned char compReg[32],
                                SIZE_T *pcbOut);

/* The DXBC input-signature component type (2 = sint, 1 = uint, 0 = leave as
 * built) a VS register needs for a vertex element of the given DXGI format,
 * the per-register input to tritonReconcileVsInputSig. */
unsigned char tritonDxbcFormatCompType(UINT dxgiFormat);

typedef struct TritonDxbcSig {
    UINT Stream;
    UINT Register;
    UINT SemanticIndex;
    UINT Mask;          /* 4-bit xyzw component mask */
    char Name[64];
} TritonDxbcSig;

/* Parse the input (ISG1/ISGN) or output (OSG1/OSG5/OSGN) signature of a
 * DXBC or DXIL container into pOut.  Returns the number of entries written;
 * 0 when the container or the chunk is malformed. */
UINT tritonDxbcParseSig(const void *pBytecode, SIZE_T cbBytecode, BOOL fInput,
                        TritonDxbcSig *pOut, UINT maxEntries);

/* The semantic name of the input-signature entry for register reg, pointing
 * into the container (valid for its lifetime), or NULL.  *pSemIndex receives
 * its semantic index. */
const char *tritonDxbcInputSemantic(const void *pBytecode, SIZE_T cbBytecode,
                                    UINT reg, UINT *pSemIndex);

#ifdef __cplusplus
}
#endif

#endif /* TRITON_DXBC_H_INCLUDED */
