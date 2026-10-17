/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Host addresses in a 32-bit guest.  A D3D12 CPU descriptor handle is an
 * address in the host process, above 4 GiB, so a 32-bit guest cannot hold
 * it in its SIZE_T.  Each host range the guest learns (a descriptor heap's
 * start) gets a window of the same size in a 32-bit number space, and
 * handles translate by the window's offset, so the API's handle
 * arithmetic (start + i * increment) carries over unchanged.  The windows
 * are numbers, not memory: nothing dereferences a CPU descriptor handle
 * on the guest.
 */

#include "npt_cs.h"

#if UINTPTR_MAX <= UINT32_MAX

#include <stdlib.h>
#include <string.h>

/* Window bases come from [WINDOW_LO, WINDOW_HI); 0 stays null. */
#define WINDOW_LO 0x10000000ull
#define WINDOW_HI 0xffff0000ull
#define WINDOW_ALIGN 0x10000ull
/* A heap fetched without a size hint (none is created that way today). */
#define WINDOW_DEFAULT (64ull << 20)

struct window {
   uint64_t guest;
   uint64_t host;
   uint64_t size;
};

static SRWLOCK windows_lock = SRWLOCK_INIT;
/* Sorted by guest base; disjoint in both spaces. */
static struct window *windows;
static uint32_t window_count;
static uint32_t window_cap;

static _Thread_local uint64_t window_hint;

void
npt_host_address_window_hint(uint64_t size)
{
   window_hint = size;
}

/* Caller holds windows_lock.  Index of the window holding guest, or -1. */
static int
find_guest_locked(uint64_t guest)
{
   uint32_t lo = 0, hi = window_count;
   while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if (guest < windows[mid].guest)
         hi = mid;
      else if (guest >= windows[mid].guest + windows[mid].size)
         lo = mid + 1;
      else
         return (int)mid;
   }
   return -1;
}

uint64_t
npt_guest_host_address_to_wire(uint64_t guest)
{
   if (!guest)
      return 0;
   AcquireSRWLockShared(&windows_lock);
   const int i = find_guest_locked(guest);
   const uint64_t host =
      i >= 0 ? windows[i].host + (guest - windows[i].guest) : guest;
   ReleaseSRWLockShared(&windows_lock);
   if (i < 0) {
      static _Atomic bool logged;
      if (!atomic_exchange(&logged, true))
         npt_log("host address: 0x%llx is in no window",
                 (unsigned long long)guest);
   }
   return host;
}

uint64_t
npt_guest_host_address_from_wire(uint64_t wire)
{
   if (!wire)
      return 0;
   uint64_t size = window_hint ? window_hint : WINDOW_DEFAULT;
   window_hint = 0;
   size = (size + WINDOW_ALIGN - 1) & ~(WINDOW_ALIGN - 1);

   AcquireSRWLockExclusive(&windows_lock);
   uint64_t guest = 0;
   for (uint32_t i = 0; i < window_count; i++) {
      const struct window *w = &windows[i];
      if (wire >= w->host && wire - w->host < w->size) {
         guest = w->guest + (wire - w->host);
         goto out;
      }
   }

   /* First fit between the existing windows. */
   uint64_t base = WINDOW_LO;
   uint32_t at = 0;
   for (; at < window_count; at++) {
      if (windows[at].guest - base >= size)
         break;
      base = windows[at].guest + windows[at].size;
   }
   if (base + size > WINDOW_HI) {
      npt_log("host address: no room for a 0x%llx-byte window",
              (unsigned long long)size);
      goto out;
   }
   if (window_count == window_cap) {
      const uint32_t cap = window_cap ? window_cap * 2 : 32;
      struct window *grown = realloc(windows, cap * sizeof(*grown));
      if (!grown)
         goto out;
      windows = grown;
      window_cap = cap;
   }
   memmove(&windows[at + 1], &windows[at],
           (window_count - at) * sizeof(windows[0]));
   windows[at] = (struct window){ .guest = base, .host = wire, .size = size };
   window_count++;
   guest = base;
out:
   ReleaseSRWLockExclusive(&windows_lock);
   return guest;
}

void
npt_host_address_release(uint64_t guest)
{
   if (!guest)
      return;
   AcquireSRWLockExclusive(&windows_lock);
   const int i = find_guest_locked(guest);
   if (i >= 0 && windows[i].guest == guest) {
      memmove(&windows[i], &windows[i + 1],
              (window_count - (uint32_t)i - 1) * sizeof(windows[0]));
      window_count--;
   }
   ReleaseSRWLockExclusive(&windows_lock);
}

#endif
