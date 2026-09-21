@echo off
rem post a synthetic WM_HOTKEY to the m8b overlay window.
rem   %1 = hotkey id (1=quit, 2=toggle)
powershell -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0_tog.ps1" %1
