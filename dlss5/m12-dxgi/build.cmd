@echo off
:: SPDX-License-Identifier: Apache-2.0
setlocal
rem M12-DXGI: configure + build the DXGI present proxy.
rem Same VS-discovery fallback as m11-layer (COM discovery is broken here).

set "REPO=%~dp0\..\.."
set "SRC=%REPO%\dlss5\m12-dxgi"
set "BUILD=%SRC%\build"
set "BUILDNM=%SRC%\build-nmake"
set "CHAIN_DLL=%REPO%\dlss5\m11d\build\chain\Release\d5c_chain.dll"
if not exist "%CHAIN_DLL%" set "CHAIN_DLL=%REPO%\dlss5\m11d\build\Release\d5c_chain.dll"
if not exist "%CHAIN_DLL%" set "CHAIN_DLL=%REPO%\dlss5\m11d\build-nmake\chain\d5c_chain.dll"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"

echo === M12-DXGI build ===
echo started %DATE% %TIME%

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64
if not errorlevel 1 (
    "%CMAKE%" --build "%BUILD%" --config Release
    if errorlevel 1 ( set FINAL=1 & goto :done )
    if not exist "%BUILD%\Release\m12_dxgi.dll" ( set FINAL=1 & goto :done )
    if exist "%CHAIN_DLL%" copy /y "%CHAIN_DLL%" "%BUILD%\Release\" >nul
    for %%D in ("%REPO%\dlss5\m11d\build\Release" "%REPO%\dlss5\m11-layer\build\Release") do (
        if exist "%%~D\*.spv" copy /y "%%~D\*.spv" "%BUILD%\Release\" >nul
    )
    set FINAL=0
    goto :done
)

echo === VS generator unavailable (instance discovery broken) - NMake fallback ===
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 ( set FINAL=1 & goto :done )
"%CMAKE%" -S "%SRC%" -B "%BUILDNM%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 ( set FINAL=1 & goto :done )
"%CMAKE%" --build "%BUILDNM%"
if errorlevel 1 ( set FINAL=1 & goto :done )
if not exist "%BUILDNM%\m12_dxgi.dll" ( set FINAL=1 & goto :done )
if not exist "%BUILD%\Release" mkdir "%BUILD%\Release"
copy /y "%BUILDNM%\m12_dxgi.dll" "%BUILD%\Release\" >nul
if exist "%CHAIN_DLL%" copy /y "%CHAIN_DLL%" "%BUILD%\Release\" >nul
for %%D in ("%REPO%\dlss5\m11d\build\Release" "%REPO%\dlss5\m11-layer\build\Release") do (
    if exist "%%~D\*.spv" copy /y "%%~D\*.spv" "%BUILD%\Release\" >nul
)
set FINAL=0

:done
echo === build exit code: %FINAL% (ended %DATE% %TIME%) ===
exit /b %FINAL%
