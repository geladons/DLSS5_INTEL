@echo off
REM ============================================================
REM  DLSS5_INTEL - WINDOW MODE DEMO (M8b, real DLSS 5 graph)
REM  Processes ONLY the chosen window; the overlay covers just
REM  that window and follows its position. The rest of the
REM  desktop is untouched and fully usable.
REM
REM  Usage: RUN-WINDOW-DEMO.cmd "part of the window title"
REM  Example: RUN-WINDOW-DEMO.cmd anime      (the Photos window)
REM
REM  The effect is VISIBLE by design here (--gain 1.0
REM  --colorpass 1: full residual, tone + detail).
REM  Speed reality: ~0.8 s per frame at the Photos window
REM  extent (1344x1088) - about 1.2 fps. Best demo mode.
REM  Hotkeys: CTRL+ALT+X hide/show, CTRL+ALT+Q quit.
REM  Moving the window is tracked; resizing asks for a restart.
REM ============================================================
if "%~1"=="" (
  echo usage: RUN-WINDOW-DEMO.cmd "part of the window title"
  echo example: RUN-WINDOW-DEMO.cmd anime
  pause
  exit /b 1
)
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\build\Release
m8blive.exe --frames 1000000 --window "%~1" --gain 1.0 --colorpass 1 --echo-free 0
pause
