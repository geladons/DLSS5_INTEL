@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
m8proto.exe --to 0 --nots --split 1 --maxdisp 13 --dispdbg > out\_i13.txt 2>&1
echo m13 exit=%errorlevel%
m8proto.exe --to 0 --nots --split 1 --maxdisp 14 --dispdbg > out\_i14.txt 2>&1
echo m14 exit=%errorlevel%
findstr /c:"VK error" /c:"GPU-side complete" out\_i13.txt
echo --- i14:
findstr /c:"VK error" /c:"GPU-side complete" out\_i14.txt
powershell -NoProfile -Command "$l=Get-Content out\_i14.txt; ($l | Select-String '\[disp').Line | Select-Object -Last 2"
