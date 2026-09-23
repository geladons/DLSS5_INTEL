@echo off
rem Incremental m11d build over the existing NMake cache (no reconfigure).
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
"C:\Program Files\CMake\bin\cmake.exe" --build "%~dp0build-nmake" > "%~dp0\_build_inc.log" 2>&1
echo EXIT=%errorlevel% >> "%~dp0\_build_inc.log"
