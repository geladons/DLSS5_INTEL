@echo off
setlocal
cd /d "C:\Users\<owner>\Desktop\production\runtime\m11d"
m11d.exe --weights "C:\Users\<owner>\Desktop\production\weights\dlssnr-logical.safetensors" --selftest "C:\Users\<owner>\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp" > "C:\Users\<owner>\Desktop\production\logs\prod_selftest.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "C:\Users\<owner>\Desktop\production\logs\prod_selftest.log"
