@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /EHsc /std:c++17 -I"C:\VulkanSDK\1.4.357.0\Include" -Idlss5\chain tools\test_int8_loader.cpp dlss5\chain\weights.cpp dlss5\chain\arena.cpp /link /LIBPATH:"C:\VulkanSDK\1.4.357.0\Lib" vulkan-1.lib /out:tools\test_int8_loader.exe
if errorlevel 1 exit /b 1
tools\test_int8_loader.exe
