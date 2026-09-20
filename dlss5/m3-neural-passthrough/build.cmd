@echo off
setlocal
rem M3 neural pass slice: configure, build, run. Single orchestration point
rem (cmd batch - immune to the exec wrapper's PowerShell $-mangling). All output
rem (build + run console) lands in docs\m3-neural.log.

set "REPO=C:\Users\AI\Desktop\DLSS5_INTEL"
set "SRC=%REPO%\dlss5\m3-neural-passthrough"
set "BUILD=%SRC%\build"
set "LOG=%REPO%\docs\m3-neural.log"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

rem run args: outdir wiggle render_scale
set OUTDIR=out
set WIGGLE=1
set SCALE=0.55
if not "%~1"=="" set OUTDIR=%~1
if not "%~2"=="" set WIGGLE=%~2
if not "%~3"=="" set SCALE=%~3

echo === M3 neural pass slice build+run log ===> "%LOG%"
echo started %DATE% %TIME% >> "%LOG%"
echo args: outdir=%OUTDIR% wiggle=%WIGGLE% scale=%SCALE% >> "%LOG%"
"%CMAKE%" --version >> "%LOG%" 2>&1
"%VULKAN_SDK%\Bin\glslangValidator.exe" --version >> "%LOG%" 2>&1

echo ^>^> cmake configure >> "%LOG%"
"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64 >> "%LOG%" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )

echo ^>^> cmake build >> "%LOG%"
"%CMAKE%" --build "%BUILD%" --config Release >> "%LOG%" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )

if not exist "%BUILD%\Release\m3neural.exe" ( set FINAL=1 & goto :done )

echo ^>^> run neural slice >> "%LOG%"
pushd "%BUILD%\Release"
".\m3neural.exe" %OUTDIR% %WIGGLE% %SCALE% >> "%LOG%" 2>&1
set FINAL=%errorlevel%
popd
echo m3neural exit code: %FINAL% >> "%LOG%"

:done
echo. >> "%LOG%"
echo === final exit code: %FINAL% (ended %DATE% %TIME%) === >> "%LOG%"
type "%LOG%"
exit /b %FINAL%
