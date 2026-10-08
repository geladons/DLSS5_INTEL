/* SPDX-License-Identifier: Apache-2.0 */
/* =====================================================================
 * gdi_guard - WM_PAINT/WM_ERASEBKGND wndproc guard that keeps GDI-driven
 * repaints from tearing the swapchain surface (extracted stage 4).
 * ===================================================================== */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void nr_guard_window(HWND hwnd);
void nr_unguard_all(void);
/* Feed the latest written-back frame into the redirection-surface fresco
 * (paint handlers + periodic refresh keep the bitmap current so a DWM
 * splice shows live content instead of a stale snapshot). rgba_order: 0 =
 * B,G,R,A pixel bytes, 1 = R,G,B,A. */
void nr_guard_feed_surface(const void *pixels, unsigned w, unsigned h,
                           int rgba_order);

#ifdef __cplusplus
}
#endif
