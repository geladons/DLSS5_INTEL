@echo off
REM Same as RUN-DEMO.cmd, but if the screen stays static for
REM more than 30 s, a gentle cursor nudge keeps frames flowing
REM (only if you actually need continuous output on idle).
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\build\Release
m8blive.exe --frames 1000000 --wiggle-idle 30
pause
