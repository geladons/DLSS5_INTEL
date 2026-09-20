@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
echo === maxdisp 5 ===
m8proto.exe --to 0 --maxdisp 5
echo EXIT=%errorlevel%
