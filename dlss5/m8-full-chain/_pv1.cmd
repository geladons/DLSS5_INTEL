@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
m8proto.exe --to 0 --nots --split 1 --pv1 > out\_pv1.txt 2>&1
echo pv1 exit=%errorlevel%
findstr /c:"VK error" /c:"GPU-side complete" out\_pv1.txt
