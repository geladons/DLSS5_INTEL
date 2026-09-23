@echo off
setlocal
set "SRC=C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11d"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
echo VCVARS_RC=%ERRORLEVEL%
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"
"C:\Program Files\CMake\bin\cmake.exe" -S "%SRC%" -B "%SRC%\build-nmake" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
echo CMAKE_RC=%ERRORLEVEL%
