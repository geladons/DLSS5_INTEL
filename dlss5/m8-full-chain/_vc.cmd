@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain
findstr /n /c:"define VK_CHECK" /c:"VK_CHECK" main.cpp | findstr /c:"define"
powershell -NoProfile -Command "$l=Get-Content main.cpp; $i=($l | Select-String 'define VK_CHECK').LineNumber; $l[$i-1]"
