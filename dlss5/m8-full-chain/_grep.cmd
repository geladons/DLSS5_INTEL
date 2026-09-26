@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain
findstr /n /c:"g_dispCount" main.cpp
echo ---
findstr /n /c:"g_maxDisp) return" main.cpp
