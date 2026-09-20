@echo off
setlocal enabledelayedexpansion
rem M6a safetensors weights loader build+run orchestrator (cmd batch: immune
rem to the exec wrapper's PowerShell $-mangling). Builds m6loader and runs it
rem against the extracted DLSS 5 weights, logging everything.

set "REPO=C:\Users\AI\Desktop\DLSS5_INTEL"
set "SRC=%REPO%\dlss5\m6-weights-loader"
set "BUILD=%SRC%\build"
set "LOG=%REPO%\docs\m6a-loader.log"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo === M6a safetensors weights loader build+run log ===> "%LOG%"
echo started %DATE% %TIME% >> "%LOG%"
"%CMAKE%" --version >> "%LOG%" 2>&1

echo ^>^> cmake configure >> "%LOG%"
"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64 >> "%LOG%" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )

echo ^>^> cmake build >> "%LOG%"
"%CMAKE%" --build "%BUILD%" --config Release >> "%LOG%" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )

if not exist "%BUILD%\Release\m6loader.exe" ( set FINAL=1 & goto :done )

echo ^>^> run m6loader (verify + stats) >> "%LOG%"
pushd "%BUILD%\Release"
".\m6loader.exe" --verify --stats >> "%LOG%" 2>&1
set FINAL=!errorlevel!
popd
echo m6loader exit code: !FINAL! >> "%LOG%"

:done
echo. >> "%LOG%"
echo === final exit code: %FINAL% (ended %DATE% %TIME%) === >> "%LOG%"
type "%LOG%"
exit /b %FINAL%
