@echo off
REM ============================================================
REM  DLSS5_INTEL - LIVE DEMO (M8b: real DLSS 5 graph, 71 blocks)
REM  Your desktop, processed in real time by the full DLSSNR
REM  graph on the Intel Arc Pro B50.
REM
REM  Event-driven + paced: up to ~15 fps while the screen
REM  changes, ~1 Hz refresh when idle, GPU nearly idle on a
REM  static desktop.
REM
REM  Hotkeys:  CTRL+ALT+X = hide/show the overlay (work mode)
REM            CTRL+ALT+Q = quit
REM  NOTE: the demo overlay is fullscreen and topmost - normal
REM  windows open BEHIND it. Press CTRL+ALT+X to hide it and
REM  use the desktop normally; press again to resume.
REM  Close this window or press Ctrl+C to stop.
REM ============================================================
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\build\Release
m8blive.exe --frames 1000000
pause
