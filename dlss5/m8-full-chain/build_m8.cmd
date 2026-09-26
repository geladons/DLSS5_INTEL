@echo off
setlocal enabledelayedexpansion
rem M8a full-chain build+run. cmd batch: immune to exec wrapper $-mangling.
rem Usage: build_m8.cmd [--no-run]

set "REPO=%~dp0\..\.."
set "SRC=%REPO%\dlss5\m8-full-chain"
set "BUILD=%SRC%\build"
set "LOG=%REPO%\docs\m8-full-chain.log"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo === M8a full-chain build+run log ===> "%LOG%"
echo started %DATE% %TIME% >> "%LOG%"
"%CMAKE%" --version >> "%LOG%" 2>&1

echo ^>^> cmake configure >> "%LOG%"
"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64 >> "%LOG%" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )

echo ^>^> cmake build >> "%LOG%"
"%CMAKE%" --build "%BUILD%" --config Release >> "%LOG%" 2>&1
if errorlevel 1 ( set FINAL=2 & goto :done )

if not exist "%BUILD%\Release\m8proto.exe" ( set FINAL=3 & goto :done )

if /i "%~1"=="--no-run" ( set FINAL=0 & goto :done )

echo ^>^> run m8proto >> "%LOG%"
pushd "%BUILD%\Release"
".\m8proto.exe" >> "%LOG%" 2>&1
set FINAL=!errorlevel!
popd
echo m8proto exit code: !FINAL! >> "%LOG%"

:done
echo. >> "%LOG%"
echo === final exit code: %FINAL% (ended %DATE% %TIME%) === >> "%LOG%"
type "%LOG%"
exit /b %FINAL%
