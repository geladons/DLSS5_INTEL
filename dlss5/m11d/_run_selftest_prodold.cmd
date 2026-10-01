@echo off
set D5C_NO_INT8=1
cd /d "C:\Users\<owner>\Desktop\production\runtime\m11d"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --selftest "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp" > "c:\Users\<owner>\Desktop\DLSS5_INTEL\dlss5\m11d\_selftest_prodold.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "c:\Users\<owner>\Desktop\DLSS5_INTEL\dlss5\m11d\_selftest_prodold.log"
