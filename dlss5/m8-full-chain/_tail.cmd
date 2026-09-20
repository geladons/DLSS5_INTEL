@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
powershell -NoProfile -Command "Get-Content out\_spd.txt -Tail 25"
