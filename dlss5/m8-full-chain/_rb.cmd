@echo off
setlocal
set "REPO=%~dp0\..\.."
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
cd /d %REPO%\dlss5\m8-full-chain
"%CMAKE%" --build build --config Release > _rb.log 2>&1
if errorlevel 1 ( echo BUILD_FAIL & type _rb.log & exit /b 1 )
echo BUILD_OK
dir build\Release\m8proto.exe | findstr m8proto
