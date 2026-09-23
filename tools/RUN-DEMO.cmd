@echo off
REM ============================================================
REM  DLSS5_INTEL - LIVE DEMO (M8b: real DLSS 5 graph, 71 blocks)
REM  Your desktop, processed in real time by the full DLSSNR
REM  graph on the Intel Arc Pro B50.
REM
REM  Speed reality (2026-09-22, M9b real 71-block U-Net):
REM  ~1-2 s per frame at fullscreen extent - a SLIDESHOW, not
REM  realtime yet. Kernel optimization = milestone M10 pass 2.
REM
REM  NOTE: --echo-free 0 is REQUIRED for owner viewing: the owner
REM  watches this desktop through the Moonlight/Sunshine stream, and a
REM  WDA_EXCLUDEFROMCAPTURE overlay is INVISIBLE in that capture.
REM  fbcancel (bounded +/-12/255 per frame) handles the feedback.
REM  Hotkeys:  CTRL+ALT+X = hide/show the overlay (work mode)
REM            CTRL+ALT+Q = quit
REM  NOTE: the demo overlay is fullscreen and topmost - normal
REM  windows open BEHIND it. Press CTRL+ALT+X to hide it and
REM  use the desktop normally; press again to resume.
REM  Close this window or press Ctrl+C to stop.
REM ============================================================
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\build\Release
m8blive.exe --frames 1000000 --echo-free 0
pause
