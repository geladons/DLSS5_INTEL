@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
echo === to 0 --dispdbg ===
m8proto.exe --to 0 --dispdbg 2>&1
echo EXIT=%errorlevel%
