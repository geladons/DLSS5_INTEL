@echo off
setlocal
set "REPO=%~dp0..\.."
rem M12 proxy self-test: stage proxy-as-dxgi.dll next to the test exe,
rem run it detached, then inspect run\test.log and %TEMP%\m12_dxgi.log.
rem Assumes m11d is already listening on 127.0.0.1:47990.

set "SRC=%REPO%\dlss5\m12-dxgi"
set "RUN=%SRC%\test\run"
set "EXE=%SRC%\test\build\Release\m12_test.exe"
if not exist "%EXE%" set "EXE=%SRC%\test\build-nmake\m12_test.exe"
if not exist "%RUN%" mkdir "%RUN%"
copy /y "%SRC%\build\Release\m12_dxgi.dll" "%RUN%\dxgi.dll" >nul
for %%D in ("%SRC%\build\Release" "%REPO%\dlss5\chain\build\Release" "%REPO%\dlss5\m11d\build\Release" "%REPO%\dlss5\m11-layer\build\Release") do (
    if exist "%%~D\d5c_chain.dll" copy /y "%%~D\d5c_chain.dll" "%RUN%\" >nul
    if exist "%%~D\*.spv" copy /y "%%~D\*.spv" "%RUN%\" >nul
)
if not exist "%EXE%" (
    echo m12_test.exe missing - build the test app first
    exit /b 1
)
copy /y "%EXE%" "%RUN%\m12_test.exe" >nul
del "%RUN%\test.log" 2>nul
set M12_LIVE=4
set M12_PORT=48990
set "M12_DUMP=%RUN%\dump.bmp"
if "%~1"=="--sync" (
    pushd "%RUN%"
    "%RUN%\m12_test.exe"
    set TEST_RC=%ERRORLEVEL%
    popd
    exit /b %TEST_RC%
)
pushd "%RUN%"
start "" /D "%RUN%" /min cmd /c "m12_test.exe > test.log 2>&1"
popd
echo staged in %RUN%, test started detached
exit /b 0
