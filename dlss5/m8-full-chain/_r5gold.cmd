@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe ..\..\golden.py C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors out stem
echo exit=%errorlevel%
