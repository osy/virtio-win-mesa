/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared present-stage timing primitives for the D3D11 and D3D12 present
 * paths.  A present that measures ~10 ms of wall clock says nothing about
 * which stage owns the time -- the ring submit, the fence arm, the
 * GPU-completion wait, or the flip callback -- so each path splits its own
 * stages into its own counter block.  What they share is the clock and the
 * enable, which live here.
 *
 * Everything is gated on NPT_DEBUG=present_timing: only the enabled path
 * pays for QueryPerformanceCounter.
 */

#ifndef TRITON_PRESENT_TIMING_H
#define TRITON_PRESENT_TIMING_H

#include <windows.h>

#include "npt_env.h"

static inline double
triton_pt_qpc_us(void)
{
   static double scale;   /* races are benign: every writer stores the same value */
   LARGE_INTEGER t;
   if (scale == 0.0) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      scale = 1e6 / (double)f.QuadPart;
   }
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart * scale;
}

#define TRITON_PT_NOW() (NPT_DEBUG(PRESENT_TIMING) ? triton_pt_qpc_us() : 0.0)

/* Running maximum.  A lost race only under-reports an outlier. */
static inline void
triton_pt_max(LONG64 volatile *slot, LONG64 sample)
{
   for (;;) {
      LONG64 cur = *slot;
      if (sample <= cur ||
          InterlockedCompareExchange64((LONG64 *)slot, sample, cur) == cur)
         return;
   }
}

#endif /* TRITON_PRESENT_TIMING_H */
