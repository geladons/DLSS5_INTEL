@echo off
set "REPO=%~dp0\.."
chcp 65001 >nul
REM ============================================================
REM  DLSS5_INTEL - живая демка (M4-SIMPLE, stand-in модель)
REM  Старая транспортная демка: захват -> stand-in -> композит.
REM  Реальный DLSS 5 теперь в RUN-DEMO.cmd (M8b).
REM ============================================================
cd /d %REPO%\dlss5\m4-present-simple\build\Release
m4simple.exe --frames 1000000 --scale 0.55
pause
