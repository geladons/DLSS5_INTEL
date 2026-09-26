@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe ..\..\golden.py %REPO%\work\mlxw\dlssnr-logical.safetensors out stem
echo exit=%errorlevel%
