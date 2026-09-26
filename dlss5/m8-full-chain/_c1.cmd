@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
m8proto.exe --to 0 --dispdbg > out\_c1.txt 2>&1
cd ..\..
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe chkraw3.py
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe cmp_blk.py %REPO%\work\mlxw\dlssnr-logical.safetensors build\Release\out 0
