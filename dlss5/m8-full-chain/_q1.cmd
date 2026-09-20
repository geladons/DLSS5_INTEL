@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
m8proto.exe --to 3 --maxdisp 32 --dispdbg > out\_q1.txt 2>&1
cd ..\..
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe cmp_pub.py C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors build\Release\out 1
