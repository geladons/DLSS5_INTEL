@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8b-live
findstr /n /c:"no DDA frame" /c:"wiggle" /c:"WiggleThread" main.cpp
