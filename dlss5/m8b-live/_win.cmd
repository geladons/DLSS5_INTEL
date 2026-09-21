@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live
findstr /n /c:"CreateWindow" /c:"g_hwnd" /c:"overlay" /c:"SW_SHOWNOACTIVATE" /c:"SetWindowPos" main.cpp
