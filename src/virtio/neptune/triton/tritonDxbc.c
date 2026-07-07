/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXBC container builder.
 *
 * The D3D11 DDI hands shader create handlers a raw tokenized program:
 *   pTokens[0]      = VGPU10ProgramToken (version + program type)
 *   pTokens[1]      = total length in DWORDs
 *   pTokens[2..N-1] = instruction tokens
 * plus separately-passed signature arrays.
 *
 * ID3D11Device1::CreateXxxShader wants a DXBC container (the binary
 * blob fxc/dxc produces): magic + modified-MD5 hash + version + size +
 * chunk offsets, followed by ISGN / OSGN / optional PCSG (HS/DS) / SHDR
 * chunks. This file builds that container around the DDI tokens.
 *
 * Little-endian only (mingw-w64 → Windows x64).
 */

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "triton_log.h"

/* ---------- DXBC container format ----------
 * Field offsets verified by _Static_assert at the bottom of this file. */

#define DXBC_MAGIC          0x43425844u  /* 'DXBC' */
#define DXBC_BLOB_TYPE_ISGN 0x4E475349u  /* 'ISGN' */
#define DXBC_BLOB_TYPE_OSGN 0x4E47534Fu  /* 'OSGN' */
#define DXBC_BLOB_TYPE_PCSG 0x47534350u  /* 'PCSG' */
#define DXBC_BLOB_TYPE_SHDR 0x52444853u  /* 'SHDR' */

typedef struct DXBCHeader {
    uint32_t u32DXBC;            /* DXBC_MAGIC */
    uint8_t  au8Hash[16];        /* Modified-MD5 of cbTotal-0x14 bytes from u32Version onward. */
    uint32_t u32Version;         /* 1 */
    uint32_t cbTotal;            /* Total file size in bytes, including this header. */
    uint32_t cBlob;              /* Number of blob offsets following. */
    uint32_t aBlobOffset[1];     /* [cBlob] offsets from file start to each DXBCBlobHeader. */
} DXBCHeader;

typedef struct DXBCBlobHeader {
    uint32_t u32BlobType;        /* FourCC. */
    uint32_t cbBlob;             /* Bytes of payload following this header. */
} DXBCBlobHeader;

typedef struct DXBCBlobIOSGNElement {
    uint32_t offElementName;     /* Offset of ASCIIZ name from start of IOSGN blob. */
    uint32_t idxSemantic;
    uint32_t enmSystemValue;     /* D3D_NAME / DxilProgramSigSemantic. */
    uint32_t enmComponentType;   /* 1=uint32, 2=int32, 3=float32. */
    uint32_t idxRegister;
    uint32_t maskAndUsed;        /* Bits [7:0]=Mask, [15:8]=Used/NeverWrites mask. */
} DXBCBlobIOSGNElement;

typedef struct DXBCBlobIOSGN {
    uint32_t cElement;
    uint32_t offElement;         /* 8 — element array starts immediately after this. */
    /* Followed by DXBCBlobIOSGNElement[cElement], then ASCIIZ names. */
} DXBCBlobIOSGN;

/* ---------- Modified MD5 ----------
 *
 * Standard MD5 inner loop, but `dxbcHash` uses a non-standard terminator
 * block that D3DCompiler expects. Standard MD5 padding will not produce
 * the same hash and Microsoft's runtime / dxvk validator will reject
 * the container. */

#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))
#define MD5STEP(f, w, x, y, z, data, s) \
    ( w += f(x, y, z) + (data),  w = w<<s | w>>(32-s),  w += x )

typedef struct DxbcMd5Ctx {
    uint32_t buf[4];
    uint32_t bits[2];
    uint8_t  in[64];
} DxbcMd5Ctx;

static void md5Transform(uint32_t buf[4], const uint32_t in[16])
{
    uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];

    MD5STEP(F1, a, b, c, d, in[ 0] + 0xd76aa478,  7);
    MD5STEP(F1, d, a, b, c, in[ 1] + 0xe8c7b756, 12);
    MD5STEP(F1, c, d, a, b, in[ 2] + 0x242070db, 17);
    MD5STEP(F1, b, c, d, a, in[ 3] + 0xc1bdceee, 22);
    MD5STEP(F1, a, b, c, d, in[ 4] + 0xf57c0faf,  7);
    MD5STEP(F1, d, a, b, c, in[ 5] + 0x4787c62a, 12);
    MD5STEP(F1, c, d, a, b, in[ 6] + 0xa8304613, 17);
    MD5STEP(F1, b, c, d, a, in[ 7] + 0xfd469501, 22);
    MD5STEP(F1, a, b, c, d, in[ 8] + 0x698098d8,  7);
    MD5STEP(F1, d, a, b, c, in[ 9] + 0x8b44f7af, 12);
    MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1, 17);
    MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7be, 22);
    MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122,  7);
    MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193, 12);
    MD5STEP(F1, c, d, a, b, in[14] + 0xa679438e, 17);
    MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821, 22);

    MD5STEP(F2, a, b, c, d, in[ 1] + 0xf61e2562,  5);
    MD5STEP(F2, d, a, b, c, in[ 6] + 0xc040b340,  9);
    MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51, 14);
    MD5STEP(F2, b, c, d, a, in[ 0] + 0xe9b6c7aa, 20);
    MD5STEP(F2, a, b, c, d, in[ 5] + 0xd62f105d,  5);
    MD5STEP(F2, d, a, b, c, in[10] + 0x02441453,  9);
    MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681, 14);
    MD5STEP(F2, b, c, d, a, in[ 4] + 0xe7d3fbc8, 20);
    MD5STEP(F2, a, b, c, d, in[ 9] + 0x21e1cde6,  5);
    MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6,  9);
    MD5STEP(F2, c, d, a, b, in[ 3] + 0xf4d50d87, 14);
    MD5STEP(F2, b, c, d, a, in[ 8] + 0x455a14ed, 20);
    MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905,  5);
    MD5STEP(F2, d, a, b, c, in[ 2] + 0xfcefa3f8,  9);
    MD5STEP(F2, c, d, a, b, in[ 7] + 0x676f02d9, 14);
    MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8a, 20);

    MD5STEP(F3, a, b, c, d, in[ 5] + 0xfffa3942,  4);
    MD5STEP(F3, d, a, b, c, in[ 8] + 0x8771f681, 11);
    MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122, 16);
    MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380c, 23);
    MD5STEP(F3, a, b, c, d, in[ 1] + 0xa4beea44,  4);
    MD5STEP(F3, d, a, b, c, in[ 4] + 0x4bdecfa9, 11);
    MD5STEP(F3, c, d, a, b, in[ 7] + 0xf6bb4b60, 16);
    MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70, 23);
    MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6,  4);
    MD5STEP(F3, d, a, b, c, in[ 0] + 0xeaa127fa, 11);
    MD5STEP(F3, c, d, a, b, in[ 3] + 0xd4ef3085, 16);
    MD5STEP(F3, b, c, d, a, in[ 6] + 0x04881d05, 23);
    MD5STEP(F3, a, b, c, d, in[ 9] + 0xd9d4d039,  4);
    MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5, 11);
    MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8, 16);
    MD5STEP(F3, b, c, d, a, in[ 2] + 0xc4ac5665, 23);

    MD5STEP(F4, a, b, c, d, in[ 0] + 0xf4292244,  6);
    MD5STEP(F4, d, a, b, c, in[ 7] + 0x432aff97, 10);
    MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7, 15);
    MD5STEP(F4, b, c, d, a, in[ 5] + 0xfc93a039, 21);
    MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3,  6);
    MD5STEP(F4, d, a, b, c, in[ 3] + 0x8f0ccc92, 10);
    MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47d, 15);
    MD5STEP(F4, b, c, d, a, in[ 1] + 0x85845dd1, 21);
    MD5STEP(F4, a, b, c, d, in[ 8] + 0x6fa87e4f,  6);
    MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0, 10);
    MD5STEP(F4, c, d, a, b, in[ 6] + 0xa3014314, 15);
    MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1, 21);
    MD5STEP(F4, a, b, c, d, in[ 4] + 0xf7537e82,  6);
    MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235, 10);
    MD5STEP(F4, c, d, a, b, in[ 2] + 0x2ad7d2bb, 15);
    MD5STEP(F4, b, c, d, a, in[ 9] + 0xeb86d391, 21);

    buf[0] += a; buf[1] += b; buf[2] += c; buf[3] += d;
}

static void md5Init(DxbcMd5Ctx *pCtx)
{
    pCtx->buf[0]  = 0x67452301;
    pCtx->buf[1]  = 0xefcdab89;
    pCtx->buf[2]  = 0x98badcfe;
    pCtx->buf[3]  = 0x10325476;
    pCtx->bits[0] = 0;
    pCtx->bits[1] = 0;
}

static void md5Update(DxbcMd5Ctx *pCtx, const void *pvBuf, size_t len)
{
    const uint8_t *buf = (const uint8_t *)pvBuf;
    uint32_t t = pCtx->bits[0];
    if ((pCtx->bits[0] = t + ((uint32_t)len << 3)) < t)
        pCtx->bits[1]++;
    pCtx->bits[1] += (uint32_t)(len >> 29);

    t = (t >> 3) & 0x3f;
    if (t) {
        uint8_t *p = pCtx->in + t;
        t = 64 - t;
        if (len < t) {
            memcpy(p, buf, len);
            return;
        }
        memcpy(p, buf, t);
        md5Transform(pCtx->buf, (const uint32_t *)pCtx->in);
        buf += t;
        len -= t;
    }

    if (!((uintptr_t)buf & 0x3)) {
        while (len >= 64) {
            md5Transform(pCtx->buf, (const uint32_t *)buf);
            buf += 64;
            len -= 64;
        }
    } else {
        while (len >= 64) {
            memcpy(pCtx->in, buf, 64);
            md5Transform(pCtx->buf, (const uint32_t *)pCtx->in);
            buf += 64;
            len -= 64;
        }
    }

    memcpy(pCtx->in, buf, len);
}

static void dxbcHash(const void *pvData, uint32_t cbData, uint8_t pabDigest[16])
{
    enum { kBlockSize = 64 };
    uint8_t au8BlockBuffer[kBlockSize];
    static const uint8_t s_au8Padding[kBlockSize] = {
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    DxbcMd5Ctx ctx;
    md5Init(&ctx);

    const uint8_t *pu8 = (const uint8_t *)pvData;
    size_t cbRemaining = cbData;
    size_t cbComplete  = cbData & ~(size_t)(kBlockSize - 1);
    md5Update(&ctx, pu8, cbComplete);
    pu8 += cbComplete;
    cbRemaining -= cbComplete;

    if (cbRemaining >= kBlockSize - 2 * sizeof(uint32_t)) {
        /* Two trailer blocks. */
        memcpy(&au8BlockBuffer[0],           pu8,          cbRemaining);
        memcpy(&au8BlockBuffer[cbRemaining], s_au8Padding, kBlockSize - cbRemaining);
        md5Update(&ctx, au8BlockBuffer, kBlockSize);
        memset(&au8BlockBuffer[sizeof(uint32_t)], 0, kBlockSize - 2 * sizeof(uint32_t));
    } else {
        /* One trailer block. */
        memcpy(&au8BlockBuffer[sizeof(uint32_t)], pu8, cbRemaining);
        memcpy(&au8BlockBuffer[sizeof(uint32_t) + cbRemaining],
               s_au8Padding, kBlockSize - cbRemaining - 2 * sizeof(uint32_t));
    }
    *(uint32_t *)&au8BlockBuffer[0]                         = (uint32_t)cbData << 3;
    *(uint32_t *)&au8BlockBuffer[kBlockSize - sizeof(uint32_t)] = ((uint32_t)cbData << 1) | 1u;
    md5Update(&ctx, au8BlockBuffer, kBlockSize);

    memcpy(pabDigest, ctx.buf, 16);
}

/* ---------- Byte writer ----------
 *
 * Append-only, pre-sized. Worst-case output for SM5 with 32 inputs +
 * 32 outputs + 32 patch consts is O(few KiB) of headers + cbTokens of
 * SHDR payload, all known at construction. */

typedef struct DxbcWriter {
    uint8_t *pBegin;
    uint8_t *pPtr;
    size_t   cbAlloc;
} DxbcWriter;

static int writerInit(DxbcWriter *w, size_t cbInitial)
{
    w->pBegin = (uint8_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbInitial);
    if (!w->pBegin) return 0;
    w->pPtr     = w->pBegin;
    w->cbAlloc  = cbInitial;
    return 1;
}

static void writerFree(DxbcWriter *w)
{
    if (w->pBegin) HeapFree(GetProcessHeap(), 0, w->pBegin);
    w->pBegin = w->pPtr = NULL;
    w->cbAlloc = 0;
}

static size_t writerSize(const DxbcWriter *w) { return (size_t)(w->pPtr - w->pBegin); }

static int writerReserve(DxbcWriter *w, size_t cb)
{
    return (writerSize(w) + cb <= w->cbAlloc) ? 1 : 0;
}

static void *writerAppend(DxbcWriter *w, const void *p, size_t cb)
{
    if (!writerReserve(w, cb)) return NULL;
    void *dst = w->pPtr;
    if (p) memcpy(dst, p, cb); else memset(dst, 0, cb);
    w->pPtr += cb;
    return dst;
}

/* ---------- Semantic table ----------
 *
 * The DDI's D3D10_SB_NAME values (0..22) index this table to recover the
 * ASCIIZ name and DXBC enmSystemValue (D3D_NAME). Tess factor entries
 * collapse multiple D3D10_SB_NAME enum values onto a single D3D_NAME. */

typedef struct SemanticInfo {
    const char *name;
    uint32_t    compType;
    uint32_t    dxSysValue;
} SemanticInfo;

#define COMP_UINT32  1u
#define COMP_FLOAT32 3u

/* D3D_NAME values. */
#define D3D_SV_UNDEFINED                       0
#define D3D_SV_POSITION                        1
#define D3D_SV_CLIP_DISTANCE                   2
#define D3D_SV_CULL_DISTANCE                   3
#define D3D_SV_RENDER_TARGET_ARRAY_INDEX       4
#define D3D_SV_VIEWPORT_ARRAY_INDEX            5
#define D3D_SV_VERTEX_ID                       6
#define D3D_SV_PRIMITIVE_ID                    7
#define D3D_SV_INSTANCE_ID                     8
#define D3D_SV_IS_FRONT_FACE                   9
#define D3D_SV_SAMPLE_INDEX                    10
#define D3D_SV_FINAL_QUAD_EDGE_TESSFACTOR      11
#define D3D_SV_FINAL_QUAD_INSIDE_TESSFACTOR    12
#define D3D_SV_FINAL_TRI_EDGE_TESSFACTOR       13
#define D3D_SV_FINAL_TRI_INSIDE_TESSFACTOR     14
#define D3D_SV_FINAL_LINE_DETAIL_TESSFACTOR    15
#define D3D_SV_FINAL_LINE_DENSITY_TESSFACTOR   16

static const SemanticInfo g_semInfo[] = {
    /*  0 UNDEFINED              */ { "ATTRIB",                    COMP_FLOAT32, D3D_SV_UNDEFINED                    },
    /*  1 POSITION               */ { "SV_Position",               COMP_FLOAT32, D3D_SV_POSITION                     },
    /*  2 CLIP_DISTANCE          */ { "SV_ClipDistance",           COMP_FLOAT32, D3D_SV_CLIP_DISTANCE                },
    /*  3 CULL_DISTANCE          */ { "SV_CullDistance",           COMP_FLOAT32, D3D_SV_CULL_DISTANCE                },
    /*  4 RT_ARRAY_INDEX         */ { "SV_RenderTargetArrayIndex", COMP_UINT32,  D3D_SV_RENDER_TARGET_ARRAY_INDEX    },
    /*  5 VP_ARRAY_INDEX         */ { "SV_ViewportArrayIndex",     COMP_UINT32,  D3D_SV_VIEWPORT_ARRAY_INDEX         },
    /*  6 VERTEX_ID              */ { "SV_VertexID",               COMP_UINT32,  D3D_SV_VERTEX_ID                    },
    /*  7 PRIMITIVE_ID           */ { "SV_PrimitiveID",            COMP_UINT32,  D3D_SV_PRIMITIVE_ID                 },
    /*  8 INSTANCE_ID            */ { "SV_InstanceID",             COMP_UINT32,  D3D_SV_INSTANCE_ID                  },
    /*  9 IS_FRONT_FACE          */ { "SV_IsFrontFace",            COMP_UINT32,  D3D_SV_IS_FRONT_FACE                },
    /* 10 SAMPLE_INDEX           */ { "SV_SampleIndex",            COMP_UINT32,  D3D_SV_SAMPLE_INDEX                 },
    /* 11 QUAD_U0_EDGE_TESS      */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_QUAD_EDGE_TESSFACTOR   },
    /* 12 QUAD_V0_EDGE_TESS      */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_QUAD_EDGE_TESSFACTOR   },
    /* 13 QUAD_U1_EDGE_TESS      */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_QUAD_EDGE_TESSFACTOR   },
    /* 14 QUAD_V1_EDGE_TESS      */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_QUAD_EDGE_TESSFACTOR   },
    /* 15 QUAD_U_INSIDE_TESS     */ { "SV_InsideTessFactor",       COMP_FLOAT32, D3D_SV_FINAL_QUAD_INSIDE_TESSFACTOR },
    /* 16 QUAD_V_INSIDE_TESS     */ { "SV_InsideTessFactor",       COMP_FLOAT32, D3D_SV_FINAL_QUAD_INSIDE_TESSFACTOR },
    /* 17 TRI_U0_EDGE_TESS       */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_TRI_EDGE_TESSFACTOR    },
    /* 18 TRI_V0_EDGE_TESS       */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_TRI_EDGE_TESSFACTOR    },
    /* 19 TRI_W0_EDGE_TESS       */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_TRI_EDGE_TESSFACTOR    },
    /* 20 TRI_INSIDE_TESS        */ { "SV_InsideTessFactor",       COMP_FLOAT32, D3D_SV_FINAL_TRI_INSIDE_TESSFACTOR  },
    /* 21 LINE_DETAIL_TESS       */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_LINE_DETAIL_TESSFACTOR },
    /* 22 LINE_DENSITY_TESS      */ { "SV_TessFactor",             COMP_FLOAT32, D3D_SV_FINAL_LINE_DENSITY_TESSFACTOR},
};

#define G_SEM_INFO_COUNT ((UINT)(sizeof(g_semInfo) / sizeof(g_semInfo[0])))

/* PS-output UNDEFINED → "SV_Target". */
static const SemanticInfo g_semPSOutput = { "SV_Target", COMP_FLOAT32, D3D_SV_UNDEFINED };

/* Program type extracted from VGPU10ProgramToken (= pTokens[0]). The
 * upper 16 bits hold programType: 0=PS, 1=VS, 2=GS, 3=HS, 4=DS, 5=CS. */
enum {
    PROG_PS = 0,
    PROG_VS = 1,
    PROG_GS = 2,
    PROG_HS = 3,
    PROG_DS = 4,
    PROG_CS = 5,
};

static UINT programType(const UINT *pTokens)
{
    return (pTokens[0] >> 16) & 0xFFFFu;
}

/* ---------- DDI signature entry reader ----------
 *
 * D3D10DDIARG_SIGNATURE_ENTRY (12 B) and D3D11_1DDIARG_SIGNATURE_ENTRY
 * (20 B) start with the same 12-byte head: SystemValue (4), Register
 * (4), Mask (1 + 3 B pad). The 20-byte form appends
 * RegisterComponentType (4) and MinPrecision (4). One reader handles
 * both via raw offsets. */

typedef struct DdiSig {
    UINT  systemValue;     /* D3D10_SB_NAME */
    UINT  registerIdx;
    BYTE  mask;            /* lower 4 bits = xyzw */
    UINT  componentType;   /* 0 if not provided (10.x DDI) */
} DdiSig;

static void readDdiSig(const void *base, UINT stride, UINT i, DdiSig *out)
{
    const BYTE *p = (const BYTE *)base + (size_t)i * stride;
    out->systemValue  = *(const UINT *)(p + 0);
    out->registerIdx  = *(const UINT *)(p + 4);
    out->mask         = *(const BYTE *)(p + 8);
    out->componentType = (stride >= 20) ? *(const UINT *)(p + 12) : 0;
}

/* ---------- IOSGN blob writer ----------
 *
 * Layout in memory:
 *   [DXBCBlobHeader]
 *   [DXBCBlobIOSGN  (cElement, offElement=8)]
 *   [DXBCBlobIOSGNElement * cElement]
 *   [name strings, ASCIIZ, packed]
 *   [0..3 bytes of zero padding to 4-byte alignment]
 * The blob's cbBlob field counts everything after the BlobHeader
 * (including padding). offElementName is relative to the start of the
 * DXBCBlobIOSGN (i.e. excludes the 8-byte BlobHeader).
 *
 * SemanticIndex is auto-numbered per name: user inputs come out as
 * ATTRIB0, ATTRIB1, …; PS outputs as SV_Target0, SV_Target1, …. The
 * shader-side input layout resolver reads these names back. */

static const SemanticInfo *pickSemanticInfo(UINT systemValue, UINT blobType,
                                            UINT progType)
{
    if (systemValue == 0
        && blobType   == DXBC_BLOB_TYPE_OSGN
        && progType   == PROG_PS)
        return &g_semPSOutput;

    if (systemValue < G_SEM_INFO_COUNT)
        return &g_semInfo[systemValue];

    return &g_semInfo[0]; /* unknown SystemValue → ATTRIB */
}

/* Per-blob entry cap (D3D11 IA/OM/patch-const stages top out at 32). */
#define TRITON_DXBC_MAX_SIG_ENTRIES 32u

static int writeIOSGNBlob(DxbcWriter *w, UINT blobType, UINT progType,
                          const void *pEntries, UINT cEntries, UINT stride)
{
    if (cEntries > TRITON_DXBC_MAX_SIG_ENTRIES) {
        TR_LOG("writeIOSGNBlob: cEntries=%u exceeds cap %u",
               cEntries, TRITON_DXBC_MAX_SIG_ENTRIES);
        return 0;
    }

    DXBCBlobHeader *pHdr = (DXBCBlobHeader *)writerAppend(w, NULL, sizeof(DXBCBlobHeader));
    if (!pHdr) return 0;
    pHdr->u32BlobType = blobType;
    pHdr->cbBlob = 0; /* patched at the end */

    DXBCBlobIOSGN *pIO = (DXBCBlobIOSGN *)writerAppend(w, NULL, sizeof(DXBCBlobIOSGN));
    if (!pIO) return 0;
    pIO->cElement   = cEntries;
    pIO->offElement = 8;

    DXBCBlobIOSGNElement *pElems = (DXBCBlobIOSGNElement *)writerAppend(
        w, NULL, (size_t)cEntries * sizeof(DXBCBlobIOSGNElement));
    if (cEntries && !pElems) return 0;

    const char *names[TRITON_DXBC_MAX_SIG_ENTRIES];
    for (UINT i = 0; i < cEntries; ++i) {
        DdiSig sig;
        readDdiSig(pEntries, stride, i, &sig);
        const SemanticInfo *info = pickSemanticInfo(sig.systemValue, blobType, progType);
        names[i] = info->name;

        UINT semIdx = 0;
        for (UINT j = 0; j < i; ++j)
            if (strcmp(names[j], info->name) == 0)
                ++semIdx;

        DXBCBlobIOSGNElement *e = &pElems[i];
        e->offElementName  = 0;
        e->idxSemantic     = semIdx;
        e->enmSystemValue  = info->dxSysValue;
        e->enmComponentType = (sig.componentType != 0) ? sig.componentType
                                                       : info->compType;
        e->idxRegister     = sig.registerIdx;

        UINT m = (UINT)(sig.mask & 0x0F);
        UINT used;
        if (blobType == DXBC_BLOB_TYPE_ISGN) {
            /* dxvk requires Used == Mask for input layout validation. */
            used = m;
        } else if (blobType == DXBC_BLOB_TYPE_OSGN) {
            /* NeverWrites: bits set = components NOT written. We declare
             * "all written" → NeverWrites = ~Mask & 0xF. */
            used = (~m) & 0x0Fu;
        } else {
            used = 0; /* PCSG: zero matches D3DCompile output */
        }
        e->maskAndUsed = (m & 0xFFu) | ((used & 0xFFu) << 8);
    }

    /* Emit ASCIIZ names with dedup by string equality. */
    for (UINT i = 0; i < cEntries; ++i) {
        if (pElems[i].offElementName != 0) continue;
        UINT off = (UINT)(writerSize(w) - ((const uint8_t *)pIO - w->pBegin));
        size_t cb = strlen(names[i]) + 1;
        if (!writerAppend(w, names[i], cb)) return 0;
        pElems[i].offElementName = off;
        for (UINT j = i + 1; j < cEntries; ++j) {
            if (pElems[j].offElementName == 0 && strcmp(names[j], names[i]) == 0)
                pElems[j].offElementName = off;
        }
    }

    /* Pad blob to 4-byte alignment. */
    size_t cbBody = writerSize(w) - ((const uint8_t *)pIO - w->pBegin);
    size_t pad = (4 - (cbBody & 3)) & 3;
    if (pad) {
        if (!writerAppend(w, NULL, pad)) return 0;
        cbBody += pad;
    }
    pHdr->cbBlob = (uint32_t)cbBody;
    return 1;
}

/* ---------- SHDR blob writer ----------
 * Verbatim passthrough of the token stream into a chunk payload, padded
 * to 4. The token stream is already aligned (uint32_t), so pad is
 * always 0 in practice. */

static int writeSHDRBlob(DxbcWriter *w, const UINT *pTokens, size_t cbTokens)
{
    DXBCBlobHeader *pHdr = (DXBCBlobHeader *)writerAppend(w, NULL, sizeof(DXBCBlobHeader));
    if (!pHdr) return 0;
    pHdr->u32BlobType = DXBC_BLOB_TYPE_SHDR;
    pHdr->cbBlob = 0;

    uint8_t *pStart = w->pPtr;
    if (!writerAppend(w, pTokens, cbTokens)) return 0;

    size_t pad = (4 - (cbTokens & 3)) & 3;
    if (pad && !writerAppend(w, NULL, pad)) return 0;

    pHdr->cbBlob = (uint32_t)(w->pPtr - pStart);
    return 1;
}

/* ---------- tritonBuildDxbc ----------
 *
 * Assembles the container, computes the hash, returns a HeapAlloc'd
 * buffer. The caller stores the result in TRITON_SHADER::pBytecode and
 * frees via HeapFree on destroy. */

void *tritonBuildDxbc(const UINT *pTokens, SIZE_T cbTokens,
                      const void *pInEntries,    UINT cInSigs,
                      const void *pOutEntries,   UINT cOutSigs,
                      const void *pPatchEntries, UINT cPatchSigs,
                      UINT entryStride,
                      SIZE_T *pcbOut)
{
    if (pcbOut) *pcbOut = 0;

    if (!pTokens || cbTokens < 8 || !pcbOut) {
        TR_LOG("tritonBuildDxbc: invalid args pTokens=%p cbTokens=%zu pcbOut=%p",
               (const void *)pTokens, (size_t)cbTokens, (const void *)pcbOut);
        return NULL;
    }
    if (entryStride != 12 && entryStride != 20) {
        TR_LOG("tritonBuildDxbc: bad entryStride=%u (expected 12 or 20)", entryStride);
        return NULL;
    }

    const UINT progType = programType(pTokens);

    /* Blob plan: ISGN, OSGN, [PCSG for HS/DS], SHDR. */
    const int fHullDomain = (progType == PROG_HS || progType == PROG_DS);
    const UINT cBlob = 3u + (fHullDomain ? 1u : 0u);

    /* Pre-size from actual signature counts. Per-entry budget = 24 B
     * element + 32 B for name+null+slop (longest name
     * "SV_RenderTargetArrayIndex" = 25 B + null = 26 B). Per-blob
     * overhead = BlobHeader (8) + IOSGN header (8) + 4 B alignment. */
    const size_t cSigEntries  = (size_t)cInSigs + cOutSigs + cPatchSigs;
    const size_t cbSigEntries = cSigEntries * (sizeof(DXBCBlobIOSGNElement) + 32);
    const size_t cbSigBlobs   = (size_t)cBlob * (sizeof(DXBCBlobHeader) + sizeof(DXBCBlobIOSGN) + 4);
    const size_t cbShdrBlob   = sizeof(DXBCBlobHeader) + (size_t)cbTokens + 4;
    const size_t cbHdr        = sizeof(DXBCHeader) + (cBlob - 1) * sizeof(uint32_t);
    const size_t cbInit       = cbHdr + cbSigEntries + cbSigBlobs + cbShdrBlob;

    DxbcWriter w;
    if (!writerInit(&w, cbInit)) {
        TR_LOG("tritonBuildDxbc: HeapAlloc(%zu) failed", cbInit);
        return NULL;
    }

    /* Container header. cbTotal / hash patched at finalize. */
    DXBCHeader *pHdr = (DXBCHeader *)writerAppend(&w, NULL, cbHdr);
    if (!pHdr) goto fail;
    pHdr->u32DXBC    = DXBC_MAGIC;
    pHdr->u32Version = 1;
    pHdr->cBlob      = cBlob;

    UINT iBlob = 0;

    pHdr->aBlobOffset[iBlob++] = (uint32_t)writerSize(&w);
    if (!writeIOSGNBlob(&w, DXBC_BLOB_TYPE_ISGN, progType,
                        pInEntries, cInSigs, entryStride))
        goto fail;

    pHdr->aBlobOffset[iBlob++] = (uint32_t)writerSize(&w);
    if (!writeIOSGNBlob(&w, DXBC_BLOB_TYPE_OSGN, progType,
                        pOutEntries, cOutSigs, entryStride))
        goto fail;

    if (fHullDomain) {
        pHdr->aBlobOffset[iBlob++] = (uint32_t)writerSize(&w);
        if (!writeIOSGNBlob(&w, DXBC_BLOB_TYPE_PCSG, progType,
                            pPatchEntries, cPatchSigs, entryStride))
            goto fail;
    }

    pHdr->aBlobOffset[iBlob++] = (uint32_t)writerSize(&w);
    if (!writeSHDRBlob(&w, pTokens, (size_t)cbTokens))
        goto fail;

    const uint32_t cbTotal = (uint32_t)writerSize(&w);
    pHdr->cbTotal = cbTotal;

    /* Hash covers [u32Version .. EOF]. Magic + hash field at 0..0x13
     * are excluded. */
    _Static_assert(offsetof(DXBCHeader, u32Version) == 0x14, "DXBC hash range");
    dxbcHash(&pHdr->u32Version, cbTotal - 0x14u, pHdr->au8Hash);

    void *pOut = w.pBegin;
    *pcbOut = (SIZE_T)cbTotal;
    w.pBegin = NULL;  /* transfer ownership */
    writerFree(&w);
    return pOut;

fail:
    TR_LOG("tritonBuildDxbc: assembly failed");
    writerFree(&w);
    return NULL;
}

/* ---------- tritonBuildInputSigDxbc ----------
 *
 * Builds a minimal DXBC container carrying ONLY an ISGN chunk that describes
 * exactly the supplied (name, semanticIndex, register) entries.  Used by
 * tritonResolveInputLayout: D3D11 CreateInputLayout validates the element
 * descs against an input signature, and dxvk rejects any signature input not
 * covered by an element -- so the signature handed to it must mirror the
 * layout's elements, not the (often larger) bound VS signature.  Every entry
 * is declared as a 4-component float32 with Used == Mask, which is all dxvk's
 * input-layout validator inspects.  HeapAlloc'd; caller frees via HeapFree. */
void *tritonBuildInputSigDxbc(const char *const *pNames,
                              const UINT *pSemanticIndices,
                              const UINT *pRegisters,
                              UINT cEntries, SIZE_T *pcbOut)
{
    if (pcbOut) *pcbOut = 0;
    if (!pcbOut || !pNames || !pSemanticIndices || !pRegisters
        || cEntries == 0 || cEntries > TRITON_DXBC_MAX_SIG_ENTRIES)
        return NULL;

    /* cBlob == 1: the DXBCHeader's aBlobOffset[1] already holds the one
     * offset, so cbHdr is just sizeof(DXBCHeader). */
    const size_t cbHdr  = sizeof(DXBCHeader);
    const size_t cbInit = cbHdr
        + sizeof(DXBCBlobHeader) + sizeof(DXBCBlobIOSGN)
        + (size_t)cEntries * (sizeof(DXBCBlobIOSGNElement) + 32) + 16;

    DxbcWriter w;
    if (!writerInit(&w, cbInit)) {
        TR_LOG("tritonBuildInputSigDxbc: HeapAlloc(%zu) failed", cbInit);
        return NULL;
    }

    DXBCHeader *pHdr = (DXBCHeader *)writerAppend(&w, NULL, cbHdr);
    if (!pHdr) goto fail;
    pHdr->u32DXBC        = DXBC_MAGIC;
    pHdr->u32Version     = 1;
    pHdr->cBlob          = 1;
    pHdr->aBlobOffset[0] = (uint32_t)writerSize(&w);

    DXBCBlobHeader *pBlob = (DXBCBlobHeader *)writerAppend(&w, NULL, sizeof(DXBCBlobHeader));
    if (!pBlob) goto fail;
    pBlob->u32BlobType = DXBC_BLOB_TYPE_ISGN;
    pBlob->cbBlob      = 0; /* patched below */

    DXBCBlobIOSGN *pIO = (DXBCBlobIOSGN *)writerAppend(&w, NULL, sizeof(DXBCBlobIOSGN));
    if (!pIO) goto fail;
    pIO->cElement   = cEntries;
    pIO->offElement = 8;

    DXBCBlobIOSGNElement *pElems = (DXBCBlobIOSGNElement *)writerAppend(
        &w, NULL, (size_t)cEntries * sizeof(DXBCBlobIOSGNElement));
    if (!pElems) goto fail;

    for (UINT i = 0; i < cEntries; ++i) {
        pElems[i].offElementName   = 0; /* filled when the name is emitted */
        pElems[i].idxSemantic      = pSemanticIndices[i];
        pElems[i].enmSystemValue   = 0;  /* not a system-value input */
        pElems[i].enmComponentType = 3;  /* float32; dxvk ignores this for IL */
        pElems[i].idxRegister      = pRegisters[i];
        pElems[i].maskAndUsed      = 0x0Fu | (0x0Fu << 8); /* Mask=Used=xyzw */
    }

    /* Emit ASCIIZ names (offsets relative to the IOSGN blob), deduped. */
    for (UINT i = 0; i < cEntries; ++i) {
        if (pElems[i].offElementName != 0) continue;
        const char *nm = pNames[i] ? pNames[i] : "ATTRIB";
        UINT off = (UINT)(writerSize(&w) - ((const uint8_t *)pIO - w.pBegin));
        size_t cb = strlen(nm) + 1;
        if (!writerAppend(&w, nm, cb)) goto fail;
        pElems[i].offElementName = off;
        for (UINT j = i + 1; j < cEntries; ++j) {
            const char *nj = pNames[j] ? pNames[j] : "ATTRIB";
            if (pElems[j].offElementName == 0 && strcmp(nj, nm) == 0)
                pElems[j].offElementName = off;
        }
    }

    /* Pad the blob body to a 4-byte boundary. */
    size_t cbBody = writerSize(&w) - ((const uint8_t *)pIO - w.pBegin);
    size_t pad = (4 - (cbBody & 3)) & 3;
    if (pad && !writerAppend(&w, NULL, pad)) goto fail;
    cbBody += pad;
    pBlob->cbBlob = (uint32_t)cbBody;

    const uint32_t cbTotal = (uint32_t)writerSize(&w);
    pHdr->cbTotal = cbTotal;
    dxbcHash(&pHdr->u32Version, cbTotal - 0x14u, pHdr->au8Hash);

    void *pOut = w.pBegin;
    *pcbOut = (SIZE_T)cbTotal;
    w.pBegin = NULL; /* transfer ownership to caller */
    writerFree(&w);
    return pOut;

fail:
    TR_LOG("tritonBuildInputSigDxbc: assembly failed");
    writerFree(&w);
    return NULL;
}

/* If any of these fire, the container format is misaligned and the
 * runtime / dxvk will reject every shader. */
_Static_assert(sizeof(DXBCBlobHeader)        ==  8, "DXBCBlobHeader");
_Static_assert(sizeof(DXBCBlobIOSGNElement)  == 24, "DXBCBlobIOSGNElement");
_Static_assert(sizeof(DXBCBlobIOSGN)         ==  8, "DXBCBlobIOSGN (header only)");
_Static_assert(offsetof(DXBCHeader, au8Hash)     ==  4, "DXBCHeader.au8Hash");
_Static_assert(offsetof(DXBCHeader, u32Version)  == 20, "DXBCHeader.u32Version");
_Static_assert(offsetof(DXBCHeader, cbTotal)     == 24, "DXBCHeader.cbTotal");
_Static_assert(offsetof(DXBCHeader, cBlob)       == 28, "DXBCHeader.cBlob");
_Static_assert(offsetof(DXBCHeader, aBlobOffset) == 32, "DXBCHeader.aBlobOffset");
