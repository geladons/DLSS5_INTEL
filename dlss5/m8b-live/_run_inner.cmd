@echo off
setlocal
set "BUILD=C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\build"
pushd "%BUILD%\Release"
m8blive.exe --frames 1000000 > "C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\out\toggle_test.log" 2>&1
popd
