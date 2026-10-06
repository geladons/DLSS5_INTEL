@echo off
rem Build the m11 Vulkan present layer as a 32-bit DLL (for 32-bit DXVK
rem games: GTA IV / GTA SA are x86 D3D9; DXVK turns them into x86 Vulkan
rem processes, and a 32-bit process only sees Wow6432Node implicit layers).
rem No libvulkan needed: the layer resolves everything via the loader chain
rem (same as the x64 CMake build). Output: x86\nr_layer_win32.dll.
setlocal
set "REPO=%~dp0\..\.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (
    echo [FAIL] vcvars32.bat
    exit /b 1
)
set SRC=%REPO%\dlss5\m11-layer
set OUT=%SRC%\x86
if not exist %OUT% mkdir %OUT%
cl /nologo /std:c11 /O2 /c /I"C:\VulkanSDK\1.4.357.0\Include" /I"%REPO%\dlss5\chain" ^
   "%SRC%\nr_layer_win.c" /Fo"%OUT%\nr_layer_win32.obj" 2>&1
if errorlevel 1 (
    echo [FAIL] cl x86 nr_layer_win
    exit /b 1
)
cl /nologo /std:c11 /O2 /c /I"C:\VulkanSDK\1.4.357.0\Include" /I"%REPO%\dlss5\chain" ^
   "%SRC%\nr_framegen.c" /Fo"%OUT%\nr_framegen.obj" 2>&1
if errorlevel 1 (
    echo [FAIL] cl x86 nr_framegen
    exit /b 1
)
cl /nologo /std:c11 /O2 /c /I"C:\VulkanSDK\1.4.357.0\Include" /I"%REPO%\dlss5\chain" ^
   "%SRC%\nr_degrade.c" /Fo"%OUT%\nr_degrade.obj" 2>&1
if errorlevel 1 (
    echo [FAIL] cl x86 nr_degrade
    exit /b 1
)
cl /nologo /std:c11 /O2 /c /I"C:\VulkanSDK\1.4.357.0\Include" /I"%REPO%\dlss5\chain" ^
   "%SRC%\nr_util.c" /Fo"%OUT%\nr_util.obj" 2>&1
if errorlevel 1 (
    echo [FAIL] cl x86 nr_util
    exit /b 1
)
cl /nologo /std:c11 /O2 /c /I"C:\VulkanSDK\1.4.357.0\Include" /I"%REPO%\dlss5\chain" ^
   "%SRC%\daemon_client.c" /Fo"%OUT%\daemon_client.obj" 2>&1
if errorlevel 1 (
    echo [FAIL] cl x86 daemon_client
    exit /b 1
)
cl /nologo /std:c11 /O2 /c /I"C:\VulkanSDK\1.4.357.0\Include" /I"%REPO%\dlss5\chain" ^
   "%SRC%\gdi_guard.c" /Fo"%OUT%\gdi_guard.obj" 2>&1
if errorlevel 1 (
    echo [FAIL] cl x86 gdi_guard
    exit /b 1
)
cl /nologo /std:c11 /O2 /c /I"C:\VulkanSDK\1.4.357.0\Include" /I"%REPO%\dlss5\chain" ^
   "%SRC%\nr_ui.c" /Fo"%OUT%\nr_ui.obj" 2>&1
if errorlevel 1 (
    echo [FAIL] cl x86 nr_ui
    exit /b 1
)
cl /nologo /std:c11 /O2 /c /I"C:\VulkanSDK\1.4.357.0\Include" /I"%REPO%\dlss5\chain" ^
   "%SRC%\nr_async.c" /Fo"%OUT%\nr_async.obj" 2>&1
if errorlevel 1 (
    echo [FAIL] cl x86 nr_async
    exit /b 1
)
cl /nologo /std:c11 /O2 /c /I"C:\VulkanSDK\1.4.357.0\Include" /I"%REPO%\dlss5\chain" ^
   "%SRC%\nr_residual.c" /Fo"%OUT%\nr_residual.obj" 2>&1
if errorlevel 1 (
    echo [FAIL] cl x86 nr_residual
    exit /b 1
)
link /nologo /DLL /DEF:"%SRC%\nr_layer_win.def" ws2_32.lib user32.lib ^
   "%OUT%\nr_layer_win32.obj" "%OUT%\nr_framegen.obj" "%OUT%\nr_degrade.obj" ^
   "%OUT%\nr_util.obj" "%OUT%\daemon_client.obj" "%OUT%\gdi_guard.obj" ^
   "%OUT%\nr_ui.obj" "%OUT%\nr_async.obj" "%OUT%\nr_residual.obj" ^
   /OUT:"%OUT%\nr_layer_win32.dll" /PDB:"%OUT%\nr_layer_win32.pdb" 2>&1
if errorlevel 1 (
    echo [FAIL] link x86
    exit /b 1
)
echo [OK] x86 layer: %OUT%\nr_layer_win32.dll
