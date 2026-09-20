@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain
call _rb.cmd >nul
cd build\Release
m8proto.exe --to 3 --maxdisp 35 --dispdbg > out\_q2.txt 2>&1
cd ..\..
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe cmp_b2loop.py C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors build\Release\out
