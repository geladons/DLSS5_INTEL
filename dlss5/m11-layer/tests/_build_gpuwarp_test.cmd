@echo off
rem Build the gpuwarp offline harness (x64 dev tool - links vulkan-1.lib,
rem unlike the layer which resolves via the loader chain).
setlocal
set "REPO=%~dp0..\..\.."
set "SRC=%REPO%\dlss5\m11-layer"
set "OUT=%SRC%\tests\build"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [FAIL] vcvars64 & exit /b 1 )
call "%SRC%\shaders\_embed_spv.cmd"
if errorlevel 1 ( echo [FAIL] gpuwarp spv embed & exit /b 1 )
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c11 /O2 /I"C:\VulkanSDK\1.4.357.0\Include" /I"%REPO%\dlss5\chain" ^
   "%SRC%\tests\gpuwarp_test.c" "%SRC%\nr_gpuwarp.c" "%SRC%\nr_residual.c" ^
   "%SRC%\nr_warp.c" "%SRC%\nr_util.c" /Fe"%OUT%\gpuwarp_test.exe" ^
   /Fo"%OUT%\\" /link "C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib" ws2_32.lib user32.lib 2>&1
if errorlevel 1 ( echo [FAIL] cl gpuwarp_test & exit /b 1 )
echo [OK] %OUT%\gpuwarp_test.exe
exit /b 0
