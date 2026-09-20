@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
echo === block 1 (maxdisp 32) ===
m8proto.exe --to 3 --maxdisp 32 --dispdbg > out\_p1.txt 2>&1
cd ..\..
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe cmp_blk.py C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors build\Release\out 1
cd build\Release
echo === block 2 (maxdisp 46) ===
m8proto.exe --to 3 --maxdisp 46 --dispdbg > out\_p2.txt 2>&1
cd ..\..
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe cmp_blk.py C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors build\Release\out 2
