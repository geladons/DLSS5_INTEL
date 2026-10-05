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

static const WCHAR NR_GUARD_PROP[] = L"NR_OLD_WNDPROC";
static HWND g_guard_hwnds[8];
static int g_guard_count;

static LRESULT CALLBACK nr_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
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
    unlock();
}
