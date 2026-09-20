@echo off
setlocal
rem M5-ZEROCOPY: configure + build only. Run orchestration is separate
rem (runm5s.cmd) so multiple runs append to docs\m5-zerocopy.log.
rem cmd batch - immune to the exec wrapper's PowerShell $-mangling.

set "REPO=C:\Users\AI\Desktop\DLSS5_INTEL"
set "SRC=%REPO%\dlss5\m5-zerocopy"
set "BUILD=%SRC%\build"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo === M5-ZEROCOPY build ===
echo started %DATE% %TIME%
"%CMAKE%" --version
"%VULKAN_SDK%\Bin\glslangValidator.exe" --version | findstr /i version

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64
if errorlevel 1 ( set FINAL=1 & goto :done )

"%CMAKE%" --build "%BUILD%" --config Release
if errorlevel 1 ( set FINAL=1 & goto :done )

if not exist "%BUILD%\Release\m5zerocopy.exe" ( set FINAL=1 & goto :done )
set FINAL=0

:done
echo === build exit code: %FINAL% (ended %DATE% %TIME%) ===
exit /b %FINAL%
