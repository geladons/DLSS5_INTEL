@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain
findstr /n /c:"g_dispCount" main.cpp
echo ---
findstr /n /c:"g_maxDisp) return" main.cpp
