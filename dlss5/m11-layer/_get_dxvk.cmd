@echo off
rem Fetch DXVK (d3d9/dxgi DLLs) for the 32-bit DX9 game path
rem (GTA IV / GTA SA are x86 D3D9; DXVK turns them into x86 Vulkan
rem processes that the 32-bit m11 implicit layer feeds to m11d).
rem DXVK binaries are NOT committed to git; run this to re-create
rem dlss5\m11-layer\dxvk\x32 + x64. Requires curl + tar (Win10+).
setlocal
set "REPO=%~dp0\..\.."
set VER=3.1.1
set DIR=%REPO%\dlss5\m11-layer\dxvk
if not exist %DIR% mkdir %DIR%
curl -sL -o %DIR%\dxvk-%VER%.tar.gz https://github.com/doitsujin/dxvk/releases/download/v%VER%/dxvk-%VER%.tar.gz
if errorlevel 1 (echo [FAIL] curl & exit /b 1)
tar -xzf %DIR%\dxvk-%VER%.tar.gz -C %DIR%
if errorlevel 1 (echo [FAIL] tar & exit /b 1)
mkdir %DIR%\x32 %DIR%\x64 2>nul
copy /y %DIR%\dxvk-%VER%\x32\d3d9.dll %DIR%\x32\ >nul
copy /y %DIR%\dxvk-%VER%\x32\dxgi.dll %DIR%\x32\ >nul
copy /y %DIR%\dxvk-%VER%\x64\d3d9.dll %DIR%\x64\ >nul
copy /y %DIR%\dxvk-%VER%\x64\dxgi.dll %DIR%\x64\ >nul
rmdir /s /q %DIR%\dxvk-%VER%
del %DIR%\dxvk-%VER%.tar.gz
echo [OK] DXVK %VER% staged in %DIR%\x32 + x64
