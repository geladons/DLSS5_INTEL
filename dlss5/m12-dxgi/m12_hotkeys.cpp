// m12_hotkeys.h - owner hotkeys for the m12 dxgi proxy (see m12_hotkeys.h).
#include "m12_hotkeys.h"

#include "m12_hook.h"
#include "m12_log.h"

#include <windows.h>

namespace {

bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void pause_flag_path(char *out, DWORD cap)
{
    out[0] = 0;
    if (!GetTempPathA(cap, out)) return;
    if (lstrlenA(out) + 16 > (int)cap) out[0] = 0;
    else lstrcatA(out, "m12_pause.flag");
}

bool pause_flag_set()
{
    char path[MAX_PATH];
    pause_flag_path(path, MAX_PATH);
    return path[0] && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

void toggle_pause()
{
    char path[MAX_PATH];
    pause_flag_path(path, MAX_PATH);
    if (!path[0]) return;
    if (pause_flag_set()) {
        DeleteFileA(path);
        m12_logf("hotkey CTRL+ALT+X: processing RESUMED");
    } else {
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        m12_logf("hotkey CTRL+ALT+X: processing PAUSED");
    }
}

DWORD WINAPI hotkey_thread(LPVOID)
{
    bool prev_x = false, prev_q = false;
    for (;;) {
        bool mod = key_down(VK_CONTROL) && key_down(VK_MENU);
        bool x = mod && key_down('X');
        bool q = mod && key_down('Q');
        if (x && !prev_x) toggle_pause();
        if (q && !prev_q) {
            m12_logf("hotkey CTRL+ALT+Q: detach requested, restoring vtables");
            m12_hook_restore_all();
            break;
        }
        prev_x = x;
        prev_q = q;
        Sleep(50);
    }
    return 0;
}

}  // namespace

void m12_hotkeys_ensure_started()
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, hotkey_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else m12_logf("hotkey thread failed to start (err %lu)", GetLastError());
}
