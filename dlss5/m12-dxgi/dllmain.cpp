// SPDX-License-Identifier: Apache-2.0
// dllmain.cpp - entry point of the m12 dxgi proxy.
#include "m12_hook.h"
#include "m12_dx12.h"
#include "m12_log.h"

#include <windows.h>

// First-light marker: pure kernel32, no CRT, no locks. Tells us from the
// log whether the process even reaches our DllMain.
static void boot_marker(const char *msg)
{
    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path)) return;
    wcscat_s(path, MAX_PATH, L"m12_boot.log");
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, msg, (DWORD)lstrlenA(msg), &n, NULL);
    CloseHandle(f);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        boot_marker("dllmain process attach\n");
        DisableThreadLibraryCalls(hinst);
        boot_marker("disabled thread calls\n");
        m12_logf("dllmain process attach");
        boot_marker("logf returned\n");
        m12_hook_init();
        boot_marker("hook init done\n");
    } else if (reason == DLL_PROCESS_DETACH) {
        m12_dx12_shutdown_inprocess();
        m12_logf("dllmain process detach");
    }
    return TRUE;
}
