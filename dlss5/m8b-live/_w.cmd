@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live
findstr /n /c:"no DDA frame" /c:"wiggle" /c:"WiggleThread" main.cpp
