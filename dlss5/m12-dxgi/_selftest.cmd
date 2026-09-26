@echo off
setlocal
set "REPO=%~dp0\..\.."
rem M12 proxy self-test: stage proxy-as-dxgi.dll next to the test exe,
rem run it detached, then inspect run\test.log and %TEMP%\m12_dxgi.log.
rem Assumes m11d is already listening on 127.0.0.1:47990.

set "SRC=%REPO%\dlss5\m12-dxgi"
set "RUN=%SRC%\test\run"
set "EXE=%SRC%\test\build\Release\m12_test.exe"
if not exist "%EXE%" set "EXE=%SRC%\test\build-nmake\m12_test.exe"
if not exist "%RUN%" mkdir "%RUN%"
copy /y "%SRC%\build\Release\m12_dxgi.dll" "%RUN%\dxgi.dll" >nul
if not exist "%EXE%" (
    echo m12_test.exe missing - build the test app first
    exit /b 1
)
copy /y "%EXE%" "%RUN%\m12_test.exe" >nul
del "%RUN%\test.log" 2>nul
set M12_LIVE=4
set "M12_DUMP=%RUN%\dump.bmp"
start "" /min cmd /c "cd /d %RUN% && m12_test.exe > test.log 2>&1"
echo staged in %RUN%, test started detached
exit /b 0
