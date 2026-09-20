@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain
call _rb.cmd >nul
cd build\Release
m8proto.exe --to 0 --dispdbg > out\_q3.txt 2>&1
cd ..\..
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe cmp_vec.py C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors build\Release\out
