@echo off
setlocal
rem M8B-LIVE: configure + build only. Run orchestration is separate
rem (runm8b.cmd) so multiple runs append to docs\m8b-live.log.
rem cmd batch - immune to the exec wrapper's PowerShell $-mangling.

set "REPO=C:\Users\AI\Desktop\DLSS5_INTEL"
set "SRC=%REPO%\dlss5\m8b-live"
set "BUILD=%SRC%\build"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo === M8B-LIVE build ===
echo started %DATE% %TIME%
"%CMAKE%" --version
"%VULKAN_SDK%\Bin\glslangValidator.exe" --version | findstr /i version

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64
if errorlevel 1 ( set FINAL=1 & goto :done )

"%CMAKE%" --build "%BUILD%" --config Release
if errorlevel 1 ( set FINAL=1 & goto :done )

if not exist "%BUILD%\Release\m8blive.exe" ( set FINAL=1 & goto :done )
set FINAL=0

:done
echo === build exit code: %FINAL% (ended %DATE% %TIME%) ===
exit /b %FINAL%
