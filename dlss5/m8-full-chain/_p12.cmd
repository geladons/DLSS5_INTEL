@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
echo === block 1 (maxdisp 32) ===
m8proto.exe --to 3 --maxdisp 32 --dispdbg > out\_p1.txt 2>&1
cd ..\..
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe cmp_blk.py %REPO%\work\mlxw\dlssnr-logical.safetensors build\Release\out 1
cd build\Release
echo === block 2 (maxdisp 46) ===
m8proto.exe --to 3 --maxdisp 46 --dispdbg > out\_p2.txt 2>&1
cd ..\..
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe cmp_blk.py %REPO%\work\mlxw\dlssnr-logical.safetensors build\Release\out 2
