@echo off
setlocal
set "SRC=C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain"
set "BUILD=%SRC%\build"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "PATH=C:\VulkanSDK\1.4.357.0\Bin;%PATH%"
"%CMAKE%" --build "%BUILD%" --config Release
exit /b %errorlevel%
