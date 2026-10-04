// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - OverlayControl implementation (see overlay_control.h). Window
// procs and the hotkey drain are verbatim moves from main.cpp.
// ============================================================================
#include "overlay_control.h"

#include <cstdio>

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI OverlayMsgPumpThread(LPVOID) {
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

static std::atomic<bool>* s_ctrlStop = nullptr;
static BOOL WINAPI OverlayCtrlHandler(DWORD) {
    if (s_ctrlStop) s_ctrlStop->store(true);
    return TRUE;
}

void OverlayControl::installCtrlHandler() {
    s_ctrlStop = &stop;
    SetConsoleCtrlHandler(OverlayCtrlHandler, TRUE);
}

void OverlayControl::registerHotkeys() {
    // Test note: this host drops injected modifiers, so tests PostMessage the
    // WM_HOTKEY (0x0312) directly to our own window — same code path.
    HWND regHwnd = hwnd ? hwnd : nullptr;
    bool okQ = RegisterHotKey(regHwnd, HK_QUIT, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q') != FALSE;
    bool okX = RegisterHotKey(regHwnd, HK_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'X') != FALSE;
    if (!okQ) std::fprintf(stderr, "[warn] RegisterHotKey CTRL+ALT+Q failed: %lu\n", GetLastError());
    if (!okX) std::fprintf(stderr, "[warn] RegisterHotKey CTRL+ALT+X failed: %lu\n", GetLastError());
    if (okQ || okX)
        std::printf("[hotkey] registered: CTRL+ALT+Q=quit CTRL+ALT+X=toggle overlay (%s%s)\n",
                    okQ ? "Q" : "", okX ? "X" : "");
}

void OverlayControl::drainHotkeys() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_HOTKEY) {
            if (m.wParam == HK_QUIT) {
                std::printf("[hotkey] CTRL+ALT+Q - quit requested (clean shutdown)\n");
                stop.store(true);
                quitHotkey.store(true);
            } else if (m.wParam == HK_TOGGLE) {
                bool h = !hidden.load();
                hidden.store(h);
                // HIDE: drop the overlay immediately, desktop goes native.
                // SHOW: defer the ShowWindow to the live loop — it first
                // processes the current capture (kept warm while hidden)
                // with a reseeded fbcancel, presents, THEN shows the window
                // (no stale launch-time frame flash). Without this the
                // swapchain still held the pre-hide image and the settle
                // gate (capture==our own stale output) latched forever.
                if (h) {
                    if (hwnd) ShowWindow(hwnd, SW_HIDE);
                } else {
                    resumeRequested.store(true);
                }
                std::printf("[hotkey] CTRL+ALT+X - overlay %s: processing %s, desktop %s\n",
                            h ? "HIDDEN" : "SHOWN",
                            h ? "PAUSED (warm capture only, no chain/present)"
                              : "RESUMED (fresh capture + fbcancel reseed)",
                            h ? "NATIVE (feedback loop dead)" : "ENHANCED");
            }
        } else if (m.message == WM_QUIT) {
            stop.store(true);
        } else {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}
