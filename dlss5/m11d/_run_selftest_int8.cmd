@echo off
setlocal
cd /d "%~dp0build\Release"
echo === Running Selftest with INT8 === > "%~dp0_selftest_int8.log"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --selftest "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp" >> "%~dp0_selftest_int8.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%~dp0_selftest_int8.log"
copy /y out\selftest_out.bmp out\selftest_int8.bmp >nul
copy /y out\live_head.bin out\live_head_int8.bin >nul
