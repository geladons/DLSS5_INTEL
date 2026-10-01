@echo off
setlocal
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_FUSION=1"
set "D5C_INT8_ATTN=-1"
set "D5C_INT8_ATTN_QKV=30"
set "D5C_INT8_ATTN_PROJ=-1"
cd /d "%~dp0build\Release"
echo === Running Selftest with QKV=30, PROJ=-1 === > "%~dp0_selftest_qkv70.log"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --selftest "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp" >> "%~dp0_selftest_qkv70.log" 2>&1
copy /y out\selftest_out.bmp out\selftest_qkv70.bmp >nul
copy /y out\live_head.bin out\live_head_qkv70.bin >nul
