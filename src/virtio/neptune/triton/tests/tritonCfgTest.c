/* SPDX-License-Identifier: MIT */

#include <windows.h>
#include <stdio.h>

static int win32Error(const char *operation)
{
    fprintf(stderr, "%s failed: %lu\n", operation, GetLastError());
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: triton_cfg_test <absolute UMD path>\n");
        return 1;
    }
    if (GetModuleHandleW(argv[1])) {
        fprintf(stderr, "The UMD must not be loaded before enabling strict CFG\n");
        return 1;
    }

    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY policy = {0};
    if (!GetProcessMitigationPolicy(GetCurrentProcess(), ProcessControlFlowGuardPolicy,
                                    &policy, sizeof(policy)))
        return win32Error("GetProcessMitigationPolicy");
    if (!policy.EnableControlFlowGuard) {
        fprintf(stderr, "CFG must be enabled for the test executable\n");
        return 1;
    }

    policy.StrictMode = 1;
    if (!SetProcessMitigationPolicy(ProcessControlFlowGuardPolicy, &policy, sizeof(policy)))
        return win32Error("SetProcessMitigationPolicy");
    if (!GetProcessMitigationPolicy(GetCurrentProcess(), ProcessControlFlowGuardPolicy,
                                    &policy, sizeof(policy)))
        return win32Error("GetProcessMitigationPolicy");
    if (!policy.EnableControlFlowGuard || !policy.StrictMode) {
        fprintf(stderr, "Strict CFG must be active before loading the UMD\n");
        return 1;
    }

    /* Load explicitly so strict CFG is active before the driver enters the process. */
    HMODULE module = LoadLibraryExW(argv[1], NULL,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return win32Error("LoadLibraryExW");
    if (!FreeLibrary(module))
        return win32Error("FreeLibrary");

    puts("Triton loads with strict CFG enabled");
    return 0;
}
