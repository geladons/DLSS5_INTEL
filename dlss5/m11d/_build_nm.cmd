@echo off
setlocal
set "REPO=%~dp0\..\.."
set "SRC=%REPO%\dlss5\m11d"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
"C:\Program Files\CMake\bin\cmake.exe" --build "%SRC%\build-nmake" > "%SRC%\_nm_build.log" 2>&1
echo BUILD_RC=%ERRORLEVEL%
findstr /C:"error" /C:"warning C" "%SRC%\_nm_build.log" | more +0
