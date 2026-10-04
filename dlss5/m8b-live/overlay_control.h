// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - OverlayControl: overlay HWND + window/console glue + the live
// flag set the loop polls (extracted from main.cpp, stage 3b). One instance
// lives in main(); the module keeps only the callbacks the Win32 API needs.
//
// CTRL+ALT+Q = clean quit (destroy device, exit 0); CTRL+ALT+X = toggle the
// overlay (hidden = capture-only warm loop: DDA kept alive, no chain/present;
// unhide forces a fresh process+present with a reseeded fbcancel BEFORE the
// window reappears). Hotkeys register on the MAIN thread: WM_HOTKEY lands in
// the main-thread queue, drained via PeekMessage on every DDA-acquire wake.
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>

enum { HK_QUIT = 1, HK_TOGGLE = 2 };

struct OverlayControl {
    HWND hwnd = nullptr;
    bool excluded = false;                     // WDA_EXCLUDEFROMCAPTURE applied
    bool echoFreeWanted = true;                // --echo-free 0|1 (default 1)
    std::atomic<bool> stop{false};
    std::atomic<bool> hidden{false};           // CTRL+ALT+X: hidden = warm loop
    std::atomic<bool> resumeRequested{false};  // hidden->shown: force fresh frame+reseed
    std::atomic<bool> quitHotkey{false};       // CTRL+ALT+Q: clean shutdown -> exit 0

    // RegisterHotKey Q/X on this thread's queue (call from the live-loop
    // thread). Logs the result.
    void registerHotkeys();

    // Drain the calling thread's message queue; applies WM_HOTKEY actions.
    // Call on every acquire wake so hotkeys stay live while DDA blocks.
    void drainHotkeys();

    // SetConsoleCtrlHandler -> stop = true on CTRL+C / close.
    void installCtrlHandler();
};

// Console control handler needs one flag reachable from a free Win32
// callback; installCtrlHandler sets it to this instance's `stop`.
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
DWORD   WINAPI OverlayMsgPumpThread(LPVOID);
