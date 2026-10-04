/* SPDX-License-Identifier: Apache-2.0 */
/* =====================================================================
 * nr_util - leaf helpers (see nr_util.h). Implementations are verbatim
 * moves from nr_layer_win.c.
 * ===================================================================== */
#include "nr_util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static CRITICAL_SECTION g_lock;
static int g_lock_ready;

void nr_lock_init(void)
{
    InitializeCriticalSection(&g_lock);
    g_lock_ready = 1;
}

void nr_lock_destroy(void)
{
    if (g_lock_ready) { DeleteCriticalSection(&g_lock); g_lock_ready = 0; }
}

void lock(void)   { if (g_lock_ready) EnterCriticalSection(&g_lock); }
void unlock(void) { if (g_lock_ready) LeaveCriticalSection(&g_lock); }

void nr_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    char path[MAX_PATH];
    if (GetTempPathA(MAX_PATH, path)) {
        strncat(path, "nr_layer_win.log", MAX_PATH - strlen(path) - 1);
        FILE *f = fopen(path, "a");
        if (f) { fputs(buf, f); fputc('\n', f); fclose(f); }
    }
    OutputDebugStringA("[nr_layer] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

uint64_t nr_now_ms(void) { return (uint64_t)GetTickCount64(); }
