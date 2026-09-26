@echo off
setlocal
rem M11D: configure + build the DLSSNR frame daemon (+ dlss5/chain module).
rem Same VS-discovery fallback as m8b-live (COM discovery is broken here).

set "REPO=%~dp0\..\.."
set "SRC=%REPO%\dlss5\m11d"
set "BUILD=%SRC%\build"
set "BUILDNM=%SRC%\build-nmake"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo === M11D build === > "%SRC%\_build.log"
echo started %DATE% %TIME% >> "%SRC%\_build.log"

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64 >> "%SRC%\_build.log" 2>&1
if not errorlevel 1 (
    "%CMAKE%" --build "%BUILD%" --config Release >> "%SRC%\_build.log" 2>&1
    if errorlevel 1 ( set FINAL=1 & goto :done )
    if not exist "%BUILD%\Release\m11d.exe" ( set FINAL=1 & goto :done )
    set FINAL=0
    goto :done
)

echo === VS generator unavailable (instance discovery broken) - NMake fallback === >> "%SRC%\_build.log"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >> "%SRC%\_build.log" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )
"%CMAKE%" -S "%SRC%" -B "%BUILDNM%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release >> "%SRC%\_build.log" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )
"%CMAKE%" --build "%BUILDNM%" >> "%SRC%\_build.log" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )
if not exist "%BUILDNM%\m11d.exe" ( set FINAL=1 & goto :done )
if not exist "%BUILD%\Release" mkdir "%BUILD%\Release"
copy /y "%BUILDNM%\m11d.exe" "%BUILD%\Release\" >nul
copy /y "%BUILDNM%\*.spv" "%BUILD%\Release\" >nul
set FINAL=0

:done
echo === build exit code: %FINAL% (ended %DATE% %TIME%) === >> "%SRC%\_build.log"
type "%SRC%\_build.log" | findstr /C:"error" /C:"FAIL" /C:"exit code"
exit /b %FINAL%
