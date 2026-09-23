// m12_log.cpp - tiny file logger for the m12 dxgi proxy (ASCII only).
// Pure kernel32 (CreateFile/WriteFile): this DLL loads as a dependency of
// arbitrary games, and the CRT-backed logger crashed in that context, so
// keep the log path free of any CRT allocation.
#include "m12_log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

static HANDLE log_handle()
{
    static HANDLE h = NULL;
    if (!h) {
        wchar_t path[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"M12_LOG", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            n = GetTempPathW(MAX_PATH, path);
            if (n == 0 || n >= MAX_PATH) return NULL;
            wcscat_s(path, MAX_PATH, L"m12_dxgi.log");
        }
        h = CreateFileW(path, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) h = NULL;
    }
    return h;
}

void m12_logf(const char *fmt, ...)
{
    static CRITICAL_SECTION cs;
    static LONG init = 0;
    if (InterlockedCompareExchange(&init, 1, 0) == 0)
        InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    HANDLE f = log_handle();
    if (f) {
        char line[1024];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = _snprintf_s(line, sizeof line, _TRUNCATE, "[%02u:%02u:%02u.%03u] ",
                            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(line + n, sizeof line - n, _TRUNCATE, fmt, ap);
        va_end(ap);
        strncat_s(line, sizeof line, "\n", _TRUNCATE);
        DWORD written = 0;
        WriteFile(f, line, (DWORD)strlen(line), &written, NULL);
    }
    LeaveCriticalSection(&cs);
}
