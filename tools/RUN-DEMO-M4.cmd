@echo off
set "REPO=%~dp0\.."
chcp 65001 >nul
REM ============================================================
REM  DLSS5_INTEL - live demo (M4-SIMPLE, stand-in model)
REM  Legacy transport demo: capture -> stand-in -> composite.
REM  The real DLSS 5 now lives in RUN-DEMO.cmd (M8b).
REM ============================================================
cd /d %REPO%\dlss5\m4-present-simple\build\Release
m4simple.exe --frames 1000000 --scale 0.55
pause
