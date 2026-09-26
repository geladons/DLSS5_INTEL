@echo off
set "REPO=%~dp0\..\..\.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo [FAIL] vcvars64 & exit /b 1)
set DIR=%REPO%\dlss5\m11-layer\d3d9_test
cl /nologo /O2 "%DIR%\vk32probe.c" /Fe:"%DIR%\vk64probe.exe" /link user32.lib kernel32.lib 2>&1
if errorlevel 1 (echo [FAIL] cl & exit /b 1)
echo [OK] vk64probe.exe
