@echo off
setlocal
set "REPO=%~dp0\..\.."
set "BUILD=%REPO%\dlss5\m8b-live\build"
pushd "%BUILD%\Release"
m8blive.exe --frames 1000000 > "%REPO%\dlss5\m8b-live\out\toggle_test.log" 2>&1
popd
