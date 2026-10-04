// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// m8b-live - Win32 helpers (see win_util.h).
#include "win_util.h"

#include <cstdio>


// ============================================================================
const char* HrName(HRESULT hr) {
    switch (hr) {
        case DXGI_ERROR_WAIT_TIMEOUT: return "DXGI_ERROR_WAIT_TIMEOUT";
        case DXGI_ERROR_ACCESS_LOST:  return "DXGI_ERROR_ACCESS_LOST";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        case DXGI_ERROR_SESSION_DISCONNECTED:    return "DXGI_ERROR_SESSION_DISCONNECTED";
        case E_ACCESSDENIED: return "E_ACCESSDENIED";
        case E_INVALIDARG:   return "E_INVALIDARG";
        case E_OUTOFMEMORY:  return "E_OUTOFMEMORY";
        default:             return "(other)";
    }
}
std::string LuidStr(const LUID& l) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%08lx:%08lx",
                  (unsigned long)l.HighPart, (unsigned long)l.LowPart);
    return buf;
}

// -------------------------------------- Sunshine-aware capture session ----
// M8c: the desktop the game runs on can MOVE (Sunshine/Moonlight virtual
// displays; adapters appear/disappear). Capture setup therefore enumerates
// ALL DXGI adapters + outputs up front, picks ONE output by a strict
// priority, and creates the D3D11 device ON that output's adapter:
//   (a) --output NAME substring (case-insensitive; "display5" matches
//       "\\.\DISPLAY5");
//   (b) attached output whose DesktopCoordinates CONTAIN the cursor (the
//       interactive session is where the user is);
//   (c) attached PRIMARY output (top-left at virtual (0,0));
//   (d) first attached output.
// Startup and ACCESS_LOST recovery share this one path (CapBuild / CapPick).
// NOTE: bool functions here must NOT use HR_CHECK (its `return 1` would
struct WinFindCtx { const char* needle; HWND found; };
static BOOL CALLBACK WinFindCb(HWND h, LPARAM lp) {
    WinFindCtx* ctx = (WinFindCtx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;
    char title[512];
    int n = GetWindowTextA(h, title, (int)sizeof(title));
    if (n <= 0) return TRUE;
    char lc[512]; char nd[512];
    for (int i = 0; i <= n && i < 511; ++i) lc[i] = (char)tolower((unsigned char)title[i]);
    int m = (int)strlen(ctx->needle);
    for (int i = 0; i < m && i < 511; ++i) nd[i] = (char)tolower((unsigned char)ctx->needle[i]);
    lc[n < 511 ? n : 511] = 0; nd[m < 511 ? m : 511] = 0;
    if (strstr(lc, nd) && GetConsoleWindow() != h) { ctx->found = h; return FALSE; }
    return TRUE;
}
HWND FindWindowByTitle(const std::string& needle) {
    WinFindCtx ctx{needle.c_str(), nullptr};
    EnumWindows(WinFindCb, (LPARAM)&ctx);
    return ctx.found;
}


bool StrContainsI(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    const size_t hl = std::strlen(hay), nl = std::strlen(needle);
    for (size_t i = 0; i + nl <= hl; ++i)
        if (_strnicmp(hay + i, needle, nl) == 0) return true;
    return false;
}

// ---------------------------------------------------------- wiggle thread --
std::atomic<bool> g_wiggleStop{false};
void WiggleThread() {
    bool dir = false;
    while (!g_wiggleStop.load()) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        in.mi.dx = dir ? 4 : -4;
        in.mi.dy = dir ? 2 : -2;
        SendInput(1, &in, sizeof(in));
        dir = !dir;
        Sleep(8);
    }
}