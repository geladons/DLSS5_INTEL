@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
echo === maxdisp 5 ===
m8proto.exe --to 0 --maxdisp 5
echo EXIT=%errorlevel%
