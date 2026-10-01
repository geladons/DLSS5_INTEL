@echo off
setlocal
set "D5C_INT8_PILOT=4"
cd /d "%~dp0build\Release"
echo === Running Selftest with D5C_INT8_PILOT=4 === > "%~dp0_selftest_pilot4.log"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --selftest "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp" >> "%~dp0_selftest_pilot4.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%~dp0_selftest_pilot4.log"
copy /y out\selftest_out.bmp out\selftest_pilot4.bmp >nul
copy /y out\live_head.bin out\live_head_pilot4.bin >nul
