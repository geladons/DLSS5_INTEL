@echo off
setlocal
set "D5C_INT8_PILOT=70"
cd /d "%~dp0build\Release"
echo === Running Selftest with D5C_INT8_PILOT=70 === > "%~dp0_selftest_pilot70.log"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --selftest "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp" >> "%~dp0_selftest_pilot70.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%~dp0_selftest_pilot70.log"
copy /y out\selftest_out.bmp out\selftest_pilot70.bmp >nul
copy /y out\live_head.bin out\live_head_pilot70.bin >nul
