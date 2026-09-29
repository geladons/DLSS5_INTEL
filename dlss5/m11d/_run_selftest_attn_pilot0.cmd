@echo off
setlocal
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_FUSION=1"
set "D5C_INT8_ATTN=0"
cd /d "%~dp0build\Release"
echo === Running Selftest with Attention Pilot 0 === > "%~dp0_selftest_attn_p0.log"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --selftest "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp" >> "%~dp0_selftest_attn_p0.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%~dp0_selftest_attn_p0.log"
copy /y out\selftest_out.bmp out\selftest_attn_p0.bmp >nul
copy /y out\live_head.bin out\live_head_attn_p0.bin >nul
