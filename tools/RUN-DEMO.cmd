@echo off
REM ============================================================
REM  DLSS5_INTEL - LIVE DEMO (M8b: real DLSS 5 graph, 71 blocks)
REM  Your desktop, processed in real time by the full DLSSNR
REM  graph on the Intel Arc Pro B50 (~12-20 fps when active,
REM  GPU idles when the screen is static).
REM
REM  Event-driven: processing happens only when the screen
REM  changes. Close this window or press Ctrl+C to stop.
REM  Processed frames land in dlss5\m8b-live\build\Release\out\
REM ============================================================
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\build\Release
m8blive.exe --frames 1000000
pause
