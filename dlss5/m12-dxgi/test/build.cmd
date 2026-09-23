@echo off
setlocal
rem M12 test app build (same VS-discovery fallback as the proxy itself).

set "SRC=C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m12-dxgi\test"
set "BUILD=%SRC%\build"
set "BUILDNM=%SRC%\build-nmake"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64
if not errorlevel 1 (
    "%CMAKE%" --build "%BUILD%" --config Release
    if errorlevel 1 exit /b 1
    exit /b 0
)
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
"%CMAKE%" -S "%SRC%" -B "%BUILDNM%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
"%CMAKE%" --build "%BUILDNM%"
exit /b %errorlevel%
