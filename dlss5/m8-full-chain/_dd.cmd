@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
echo === to 0 --dispdbg ===
m8proto.exe --to 0 --dispdbg 2>&1
echo EXIT=%errorlevel%
