@echo off
rem Build the m11 Vulkan present layer as a 32-bit DLL (for 32-bit DXVK
rem games: GTA IV / GTA SA are x86 D3D9; DXVK turns them into x86 Vulkan
rem processes, and a 32-bit process only sees Wow6432Node implicit layers).
rem No libvulkan needed: the layer resolves everything via the loader chain
rem (same as the x64 CMake build). Output: x86\nr_layer_win32.dll.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (
    echo [FAIL] vcvars32.bat
    exit /b 1
)
set SRC=C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11-layer
set OUT=%SRC%\x86
if not exist %OUT% mkdir %OUT%
cl /nologo /std:c11 /O2 /I"C:\VulkanSDK\1.4.357.0\Include" ^
   "%SRC%\nr_layer_win.c" /Fo"%OUT%\nr_layer_win32.obj" ^
   /link /DLL /DEF:"%SRC%\nr_layer_win.def" ws2_32.lib user32.lib ^
   /OUT:"%OUT%\nr_layer_win32.dll" /PDB:"%OUT%\nr_layer_win32.pdb" 2>&1
if errorlevel 1 (
    echo [FAIL] cl x86
    exit /b 1
)
echo [OK] x86 layer: %OUT%\nr_layer_win32.dll
