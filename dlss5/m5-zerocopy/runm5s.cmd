@echo off
setlocal
rem M5-ZEROCOPY run wrapper: starts ONE detached run with per-run logs.
rem NEVER run the live binary as a blocking foreground exec - this wrapper
rem uses detached start; the caller polls docs\%LABEL%.out.log + tasklist.
rem usage: runm5s.cmd "<label>" <args...>
rem (start /b keeps handle inheritance for >> redirection; per-run stdout and
rem  stderr go to separate files; binary stdout is unbuffered via setvbuf)

set "REPO=C:\Users\AI\Desktop\DLSS5_INTEL"
set "EXE=%REPO%\dlss5\m5-zerocopy\build\Release\m5zerocopy.exe"
set "OUTDIR=%REPO%\dlss5\m5-zerocopy"
set "DOCS=%REPO%\docs"
set "LABEL=%~1"
shift

if exist "%DOCS%\%LABEL%.out.log" del /f /q "%DOCS%\%LABEL%.out.log"
if exist "%DOCS%\%LABEL%.err.log" del /f /q "%DOCS%\%LABEL%.err.log"

cd /d "%OUTDIR%"
start "m5zerocopy-%LABEL%" /b "%EXE%" %1 %2 %3 %4 %5 %6 %7 %8 >> "%DOCS%\%LABEL%.out.log" 2>> "%DOCS%\%LABEL%.err.log"
echo started detached, spawn-errorlevel %ERRORLEVEL%
exit /b 0
