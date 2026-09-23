@echo off
setlocal
rem M12-DXGI: configure + build the DXGI present proxy.
rem Same VS-discovery fallback as m11-layer (COM discovery is broken here).

set "REPO=C:\Users\AI\Desktop\DLSS5_INTEL"
set "SRC=%REPO%\dlss5\m12-dxgi"
set "BUILD=%SRC%\build"
set "BUILDNM=%SRC%\build-nmake"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"

echo === M12-DXGI build ===
echo started %DATE% %TIME%

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64
if not errorlevel 1 (
    "%CMAKE%" --build "%BUILD%" --config Release
    if errorlevel 1 ( set FINAL=1 & goto :done )
    if not exist "%BUILD%\Release\m12_dxgi.dll" ( set FINAL=1 & goto :done )
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
set FINAL=0

:done
echo === build exit code: %FINAL% (ended %DATE% %TIME%) ===
exit /b %FINAL%
