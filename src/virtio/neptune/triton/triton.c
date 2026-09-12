/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DllMain.  Triton statically links the Neptune COM factory through
 * npt_entry_internal.h; there is no runtime LoadLibrary step.
 */

#include <windows.h>

#include "npt_ring.h"
#include "npt_tls.h"
#include "triton_log.h"

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInst);
        TR_LOG("DllMain DLL_PROCESS_ATTACH");
        break;
    case DLL_PROCESS_DETACH:
        TR_LOG("DllMain DLL_PROCESS_DETACH");
        /* FreeLibrary (lpvReserved NULL): the per-thread storage keys
         * carry destructors in this image; free them before it goes.
         * Process termination keeps the image mapped until the end. */
        if (!lpvReserved) {
            npt_tls_unload();
            npt_ring_unload();
        }
        break;
    default:
        break;
    }
    return TRUE;
}
