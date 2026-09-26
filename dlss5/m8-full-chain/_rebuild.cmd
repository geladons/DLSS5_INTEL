@echo off
setlocal
set "REPO=%~dp0\..\.."
set "SRC=%REPO%\dlss5\m8-full-chain"
set "BUILD=%SRC%\build"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"
rmdir /s /q "%BUILD%"
"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64 > "%SRC%\_rebuild.log" 2>&1
if errorlevel 1 ( echo CONFIGURE_FAIL & exit /b 1 )
"%CMAKE%" --build "%BUILD%" --config Release >> "%SRC%\_rebuild.log" 2>&1
if errorlevel 1 ( echo BUILD_FAIL & exit /b 2 )
echo BUILD_OK
