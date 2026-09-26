@echo off
set "REPO=%~dp0\.."
REM ============================================================
REM  DLSS5_INTEL - LIVE DEMO (calm variant)
REM  Same as RUN-DEMO.cmd, but if the screen stays static for
REM  more than 30 s, a gentle cursor nudge keeps frames flowing
REM  (only needed if you want continuous output while idle).
REM  Hotkeys: CTRL+ALT+X hide/show overlay, CTRL+ALT+Q quit.
REM ============================================================
cd /d %REPO%\dlss5\m8b-live\build\Release
m8blive.exe --frames 1000000 --wiggle-idle 30
pause
