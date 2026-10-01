@echo off
setlocal
set "D5C_INT8_W1=-1"
set "D5C_INT8_W2=70"
cd /d "%~dp0build\Release"
echo === Running Selftest with W1=-1, W2=70 (All 10 Weight2 Blocks) === > "%~dp0_selftest_w2_pilot70.log"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --selftest "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp" >> "%~dp0_selftest_w2_pilot70.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%~dp0_selftest_w2_pilot70.log"
copy /y out\selftest_out.bmp out\selftest_w2_p70.bmp >nul
copy /y out\live_head.bin out\live_head_w2_p70.bin >nul
