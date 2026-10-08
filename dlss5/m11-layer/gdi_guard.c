/* SPDX-License-Identifier: Apache-2.0 */
/* =====================================================================
 * gdi_guard - GDI repaint guard (see gdi_guard.h). Verbatim move from
 * nr_layer_win.c; uses nr_log + lock/unlock from nr_util. The wndproc is
 * also the input-routing point for the m13 in-game overlay (nr_ui.c):
 * while the shared UI section flags the overlay visible, band-region
 * input messages become ring events for the producer (eaten); hole/game
 * input flows through to the real wndproc unchanged.
 * ===================================================================== */
#include "gdi_guard.h"
#include "nr_util.h"
#include "nr_ui.h"

#include <stdlib.h>
#include <string.h>

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

static const WCHAR NR_GUARD_PROP[] = L"NR_OLD_WNDPROC";
static HWND g_guard_hwnds[8];
static int g_guard_count;

/* ---- redirection-surface fresco ----------------------------------------
 * A swapchain window still owns a legacy GDI redirection bitmap underneath.
 * DWM splices that bitmap over the live swapchain surface for a frame or two
 * whenever the window's update path gets exercised (NCPAINT/ERASEBKGND/
 * PAINT bursts, focus transitions). The guard used to eat WM_PAINT with a
 * dry BeginPaint/EndPaint, so the bitmap never received new content and the
 * splice surfaced a minutes-old snapshot - the periodic "alternate version"
 * flash on GTA IV's menu. Fix: every paint-class message and a throttled
 * opportunistic refresh paint the LATEST frame we wrote to the swapchain
 * into the bitmap, so a splice is visually identical to the live output.
 * Snapshot is pushed by the present path (nr_guard_feed_surface) right
 * before the staging buffer is copied into the swapchain image. */
static unsigned char *g_surf;
static unsigned g_surf_w, g_surf_h;
static int g_surf_valid;
static DWORD g_surf_push_tick;
static DWORD g_surf_dc_tick;

static void surf_paint(HDC hdc, int w, int h)
{
    if (!g_surf_valid) return;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)g_surf_w;
    bmi.bmiHeader.biHeight = -(LONG)g_surf_h;   /* top-down rows */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(hdc, 0, 0, w, h, 0, 0, (int)g_surf_w, (int)g_surf_h,
                  g_surf, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

/* Present-thread entry: stash the just-written frame and opportunistically
 * repaint the redirection bitmap (400 ms cadence - enough that a splice
 * shows near-current content, cheap enough to ignore). pixels = packed
 * 4-byte pixels, tightly pitched; order 0 = B,G,R,A (DIB-native). */
void nr_guard_feed_surface(const void *pixels, unsigned w, unsigned h,
                           int rgba_order)
{
    if (!pixels || !w || !h) return;
    DWORD now = GetTickCount();
    lock();
    if (g_surf_valid && g_surf_w == w && g_surf_h == h
        && now - g_surf_push_tick < 200) {
        /* Snapshot fresh enough; still fall through to the DC refresh gate
         * so a long-static stream still re-paints the bitmap on cadence. */
    } else {
        size_t need = (size_t)w * h * 4;
        if (g_surf_w != w || g_surf_h != h || !g_surf) {
            unsigned char *nb = (unsigned char *)realloc(g_surf, need);
            if (!nb) { unlock(); return; }
            g_surf = nb;
            g_surf_w = w;
            g_surf_h = h;
            g_surf_valid = 0;
        }
        if (rgba_order) {
            const unsigned char *s = (const unsigned char *)pixels;
            unsigned char *d = g_surf;
            for (size_t i = 0; i < need; i += 4) {
                d[i] = s[i + 2]; d[i + 1] = s[i + 1];
                d[i + 2] = s[i]; d[i + 3] = 255;
            }
        } else {
            memcpy(g_surf, pixels, need);
        }
        g_surf_valid = 1;
        g_surf_push_tick = now;
    }
    if (g_surf_valid && now - g_surf_dc_tick >= 400) {
        for (int i = 0; i < g_guard_count; i++) {
            HWND hwnd = g_guard_hwnds[i];
            if (!hwnd || !IsWindow(hwnd)) continue;
            RECT rc;
            if (!GetClientRect(hwnd, &rc)) continue;
            int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            if (cw <= 0 || ch <= 0) continue;
            HDC hdc = GetDC(hwnd);
            if (!hdc) continue;
            surf_paint(hdc, cw, ch);
            ReleaseDC(hwnd, hdc);
        }
        g_surf_dc_tick = now;
    }
    unlock();
}

static LRESULT CALLBACK nr_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Paint-class message trace for flash forensics: WM_PAINT/ERASEBKGND/
     * NCPAINT arriving mid-stream is the documented "splice a stale GDI
     * frame" vector. Logged with a tick so a captured flash frame can be
     * attributed to a repaint instead of a presented buffer. Kill switch:
     * NR_LAYER_GUARD_TRACE=0. */
    static int trace_enabled = -1;
    if (trace_enabled < 0) {
        const char *e = getenv("NR_LAYER_GUARD_TRACE");
        trace_enabled = !(e && !strcmp(e, "0"));
    }
    if (trace_enabled &&
        (msg == WM_PAINT || msg == WM_ERASEBKGND || msg == WM_NCPAINT ||
         msg == 0x0007 || msg == 0x000A)) /* WM_SETFOCUS, WM_ENABLE */
        nr_log("[nr_layer] wnd msg=0x%04x hwnd=%p tick=%llu", msg,
               (void *)hwnd, (unsigned long long)GetTickCount64());
    if (msg == WM_ERASEBKGND) {
        /* Fill with the latest frame rather than "pretend erased": the
         * redirected bitmap is what DWM splices over the swapchain. */
        RECT rc;
        if (g_surf_valid && GetClientRect(hwnd, &rc)) {
            lock();
            surf_paint((HDC)wp, rc.right - rc.left, rc.bottom - rc.top);
            unlock();
        }
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc && g_surf_valid) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            lock();
            surf_paint(hdc, rc.right - rc.left, rc.bottom - rc.top);
            unlock();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    /* WM_NCPAINT is traced above but passes through - eating it would
     * leave the title bar/borders unpainted in windowed mode. */
    /* Overlay routing: flags&1 in the shared header arms the per-region
     * filter. nr_ui_message returns nonzero for band-hit messages it
     * turned into ring events (eaten); hole hits, ungated keys, focus/
     * capture bookkeeping and unlisted messages fall through to the game
     * unchanged. WM_SETCURSOR is the exception that proves the rule: the
     * eaten value must be TRUE there, or the FALSE answer walks on to the
     * game's class cursor and double-draws over the sprite. */
    if (nr_ui_capture_active(hwnd) && nr_ui_message(hwnd, msg, wp, lp))
        return (msg == WM_SETCURSOR) ? TRUE : 0;
    WNDPROC prev = (WNDPROC)GetPropW(hwnd, NR_GUARD_PROP);
    /* Self-chain defense: if a stale prop ever points back at us, fall to
     * DefWindowProcW instead of recursing into ourselves forever. */
    if (prev == (WNDPROC)nr_wnd_proc) prev = NULL;
    return prev ? CallWindowProcW(prev, hwnd, msg, wp, lp)
                : DefWindowProcW(hwnd, msg, wp, lp);
}

void nr_guard_window(HWND hwnd)
{
    static int guard_enabled = -1;
    if (guard_enabled < 0) {
        const char *e = getenv("NR_LAYER_GDI_GUARD");
        guard_enabled = !(e && !strcmp(e, "0"));
    }
    if (!guard_enabled || !hwnd) return;
    lock();
    if (GetPropW(hwnd, NR_GUARD_PROP) || g_guard_count >= 8) {
        unlock();
        return;
    }
    SetLastError(0);
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                            (LONG_PTR)nr_wnd_proc);
    if (!prev && GetLastError()) {          /* swap failed outright */
        unlock();
        return;
    }
    if (prev == nr_wnd_proc) {
        /* Raced install: another surface create beat us between the prop
         * check and the swap. Restoring to "prev" (=us) is a no-op; the
         * first installer's prop still carries the real wndproc. */
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)nr_wnd_proc);
        unlock();
        return;
    }
    if (prev && !SetPropW(hwnd, NR_GUARD_PROP, (HANDLE)prev)) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)prev);  /* roll back */
        unlock();
        return;
    }
    if (prev) {
        g_guard_hwnds[g_guard_count++] = hwnd;
        nr_log("[nr_layer] GDI repaint guard armed on hwnd %p", (void *)hwnd);
    }
    unlock();

    /* NR_LAYER_NOREDIR=1: strip the window's redirection bitmap so DWM can
     * never splice a stale GDI-era copy of the client area over the live
     * swapchain surface (observed as periodic fixed-menu flashes on GTA IV).
     * Safe for Vulkan/DXVK swapchain windows; blit-model legacy paths would
     * need the bitmap, hence the opt-in flag pending field data. */
    static int noredir_enabled = -1;
    if (noredir_enabled < 0) {
        const char *e = getenv("NR_LAYER_NOREDIR");
        noredir_enabled = (e && !strcmp(e, "1"));
    }
    if (noredir_enabled) {
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex && !(ex & WS_EX_NOREDIRECTIONBITMAP)) {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                              ex | WS_EX_NOREDIRECTIONBITMAP);
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
            nr_log("[nr_layer] WS_EX_NOREDIRECTIONBITMAP set on hwnd %p",
                   (void *)hwnd);
        }
    }
}

/* Restore every armed wndproc before the DLL can be unmapped (instance
 * destroy / FreeLibrary): a queued message into a dangling nr_wnd_proc is an
 * AV inside the game. Safe to call twice. */
void nr_unguard_all(void)
{
    lock();
    /* Release the overlay first: ClipCursor must go back to normal before
     * the wndprocs below stop gating, and the shared view is unmapped
     * under the same lock that serializes ring pushes. Idempotent. */
    nr_ui_detach();
    for (int i = 0; i < g_guard_count; i++) {
        HWND hwnd = g_guard_hwnds[i];
        WNDPROC prev = (WNDPROC)GetPropW(hwnd, NR_GUARD_PROP);
        if (prev) {
            if ((WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == nr_wnd_proc)
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)prev);
            RemovePropW(hwnd, NR_GUARD_PROP);
        }
        g_guard_hwnds[i] = NULL;
    }
    g_guard_count = 0;
    free(g_surf);
    g_surf = NULL;
    g_surf_valid = 0;
    g_surf_w = g_surf_h = 0;
    unlock();
}
