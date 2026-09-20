@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
set M8_DEBUG_BIAS=1
m8proto.exe > out\_fresh1.txt 2>&1
echo EXIT=%errorlevel%
findstr /c:"bias-swz" out\_fresh1.txt
findstr /c:"steady-state" /c:"chain total" /c:"dispatches" out\_fresh1.txt
