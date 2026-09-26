@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
m8proto.exe --to 3 --dispdbg > out\_to3.txt 2>&1
echo EXIT=%errorlevel%
cd ..\..
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe cmp_b3.py %REPO%\work\mlxw\dlssnr-logical.safetensors build\Release\out 3
