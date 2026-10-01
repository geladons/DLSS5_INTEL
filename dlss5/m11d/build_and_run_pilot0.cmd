@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
cl /nologo /O2 /EHsc /std:c++17 /I"%VULKAN_SDK%\Include" dlss5\m11d\test_pilot0.cpp /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib /out:dlss5\m11d\test_pilot0.exe
if errorlevel 1 exit /b 1
dlss5\m11d\test_pilot0.exe
