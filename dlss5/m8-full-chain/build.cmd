@echo off
setlocal enabledelayedexpansion
rem M7 graph prototype (rounding unit kernel + global block 31) build+run.
rem cmd batch: immune to the exec wrapper's PowerShell $-mangling.
rem Usage: build.cmd [--no-run]

set "REPO=%~dp0\..\.."
set "SRC=%REPO%\dlss5\m7-graph-proto"
set "BUILD=%SRC%\build"
set "LOG=%REPO%\docs\m7-proto.log"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo === M7 graph prototype build+run log ===> "%LOG%"
echo started %DATE% %TIME% >> "%LOG%"
"%CMAKE%" --version >> "%LOG%" 2>&1

echo ^>^> cmake configure >> "%LOG%"
"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64 >> "%LOG%" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )

echo ^>^> cmake build >> "%LOG%"
"%CMAKE%" --build "%BUILD%" --config Release >> "%LOG%" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )

if not exist "%BUILD%\Release\m7proto.exe" ( set FINAL=1 & goto :done )

if /i "%~1"=="--no-run" ( set FINAL=0 & goto :done )

echo ^>^> run m7proto >> "%LOG%"
pushd "%BUILD%\Release"
".\m7proto.exe" >> "%LOG%" 2>&1
set FINAL=!errorlevel!
popd
echo m7proto exit code: !FINAL! >> "%LOG%"

:done
echo. >> "%LOG%"
echo === final exit code: %FINAL% (ended %DATE% %TIME%) === >> "%LOG%"
type "%LOG%"
exit /b %FINAL%
