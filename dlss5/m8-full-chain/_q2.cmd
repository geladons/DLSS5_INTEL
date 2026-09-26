@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain
call _rb.cmd >nul
cd build\Release
m8proto.exe --to 3 --maxdisp 35 --dispdbg > out\_q2.txt 2>&1
cd ..\..
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe cmp_b2loop.py %REPO%\work\mlxw\dlssnr-logical.safetensors build\Release\out
