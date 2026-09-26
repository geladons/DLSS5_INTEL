@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8b-live
findstr /n /c:"CreateWindow" /c:"g_hwnd" /c:"overlay" /c:"SW_SHOWNOACTIVATE" /c:"SetWindowPos" main.cpp
