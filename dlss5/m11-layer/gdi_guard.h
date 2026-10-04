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

#ifdef __cplusplus
}
#endif
