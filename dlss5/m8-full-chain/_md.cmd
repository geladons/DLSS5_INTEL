@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
m8proto.exe --to 3 --maxdisp %1 --dispdbg > out\_md%1.txt 2>&1
echo EXIT=%errorlevel%
cd ..\..
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe cmp_b3.py C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors build\Release\out %2
