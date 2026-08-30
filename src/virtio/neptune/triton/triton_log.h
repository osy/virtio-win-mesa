/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Logging and DDI-entry macros.
 *
 *   TR_LOG(fmt, ...)  - OutputDebugStringA debug print.
 *   TR_STUB(name)     - logs "Triton: STUB <name>" once per thunk.
 */

#ifndef TRITON_LOG_H_INCLUDED
#define TRITON_LOG_H_INCLUDED

#include <windows.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void triton_log_raw(const char *line)
{
    OutputDebugStringA(line);
}

#define TR_LOG(fmt, ...) do {                                         \
        char _trbuf[512];                                             \
        _snprintf_s(_trbuf, sizeof(_trbuf), _TRUNCATE,                \
                    "Triton: " fmt "\n", ##__VA_ARGS__);              \
        triton_log_raw(_trbuf);                                       \
    } while (0)


/* Per-DDI-call trace: compiled OUT by default.  OutputDebugStringA is a
 * RaiseException + global DBWIN handshake per call; at draw/list rates it
 * dominates real work: a loading thread can spend minutes parked
 * inside ODS.  Define TRITON_HOT_LOG to re-enable when tracing a
 * specific run. */
#ifdef TRITON_HOT_LOG
#define TR_LOG_HOT TR_LOG
#else
#define TR_LOG_HOT(fmt, ...) do { } while (0)
#endif

#define TR_STUB(name) do {                                            \
        static LONG _seen = 0;                                        \
        if (InterlockedExchange(&_seen, 1) == 0)                      \
            TR_LOG("STUB %s", (name));                                \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* TRITON_LOG_H_INCLUDED */
