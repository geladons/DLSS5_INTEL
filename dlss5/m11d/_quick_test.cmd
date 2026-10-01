@echo off
setlocal
set "D5C_INT8_PILOT=70"
cd /d "%~dp0build\Release"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 3 1920x1088 --prof "out\bench_int8_1080p.csv" > "%~dp0_quick_test.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%~dp0_quick_test.log"
