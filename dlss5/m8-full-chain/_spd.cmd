@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
m8proto.exe --to 4 --nots --split 4 --dispdbg > out\_spd.txt 2>&1
echo exit=%errorlevel%
