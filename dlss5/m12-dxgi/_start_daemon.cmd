@echo off
setlocal
set "REPO=%~dp0\..\.."
rem Start the m11d DLSSNR daemon detached (CWD must be build-nmake: the
rem shaders (.spv) live next to the exe). Log appended to work\_m11\m11d.log.
rem NOTE: 127.0.0.1:47990 coexists with Sunshine's wildcard listener on the
rem same port number (verified 2026-09-22); loopback traffic routes to m11d.

set "D=%REPO%\dlss5\m11d\build-nmake"
set "L=%REPO%\work\_m11"
if not exist "%L%" mkdir "%L%"
tasklist /FI "IMAGENAME eq m11d.exe" 2>nul | find /I "m11d.exe" >nul
if not errorlevel 1 (
    echo m11d already running
    exit /b 0
)
start "m11d" /D "%D%" /min cmd /c "m11d.exe >> %L%\m11d.log 2>&1"
echo m11d started detached
exit /b 0
