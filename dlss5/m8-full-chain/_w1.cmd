@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain
call _rb.cmd >nul
cd build\Release
m8proto.exe --to 0 --dispdbg > out\_w1.txt 2>&1
cd ..\..
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe cmp_w.py %REPO%\work\mlxw\dlssnr-logical.safetensors build\Release\out
