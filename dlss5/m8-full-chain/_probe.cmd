@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL
"C:\Program Files\Git\cmd\git.exe" log --oneline -5
echo ---STATUS---
"C:\Program Files\Git\cmd\git.exe" status --short
echo ---M8 DIR---
dir /b /o:d dlss5\m8-full-chain
echo ---SHADERS---
dir /b dlss5\m8-full-chain\shaders
echo ---MAIN.CPP TIME---
dir dlss5\m8-full-chain\main.cpp
echo ---BUILD EXE---
if exist dlss5\m8-full-chain\build\Release\m8proto.exe (dir dlss5\m8-full-chain\build\Release\m8proto.exe) else (echo NO EXE)
