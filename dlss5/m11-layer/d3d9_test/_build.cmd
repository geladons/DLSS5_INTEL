@echo off
rem Build the 32-bit D3D9 test app (see d3d9_test.c header).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (
    echo [FAIL] vcvars32.bat
    exit /b 1
)
set DIR=C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11-layer\d3d9_test
cl /nologo /O2 "%DIR%\d3d9_test.c" /Fe:"%DIR%\d3d9_test.exe" /link user32.lib gdi32.lib d3d9.lib 2>&1
if errorlevel 1 (
    echo [FAIL] cl
    exit /b 1
)
rem Stage DXVK x32 next to the exe (local DLL dir wins over system32).
copy /y "C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11-layer\dxvk\x32\d3d9.dll" "%DIR%\" >nul
copy /y "C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11-layer\dxvk\x32\dxgi.dll" "%DIR%\" >nul
echo [OK] %DIR%\d3d9_test.exe (+ dxvk d3d9.dll/dxgi.dll staged)
