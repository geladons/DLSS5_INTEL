@echo off
rem Build the 32-bit D3D9Ex reset-loop test app (DXVK next to it does the rest).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (
    echo [FAIL] vcvars32.bat
    exit /b 1
)
set SRC=C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11-layer\d3d9_test
cl /nologo /O2 "%SRC%\d3d9_test.c" /Fo"%SRC%\d3d9_test.obj" /Fe"%SRC%\d3d9_test.exe" /link user32.lib gdi32.lib 2>&1
if errorlevel 1 (
    echo [FAIL] cl d3d9_test
    exit /b 1
)
echo [OK] d3d9_test.exe
