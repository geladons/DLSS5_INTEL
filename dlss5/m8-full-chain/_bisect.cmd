@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
echo === to -1 (adapter only) ===
m8proto.exe --to -1
echo EXIT=%errorlevel%
echo === to 0 (+b0) ===
m8proto.exe --to 0
echo EXIT=%errorlevel%
