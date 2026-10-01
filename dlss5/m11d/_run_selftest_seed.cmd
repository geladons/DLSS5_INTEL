@echo off
cd /d "%~dp0build\Release"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --selftest "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp" > "%~dp0_selftest_seed.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%~dp0_selftest_seed.log"
