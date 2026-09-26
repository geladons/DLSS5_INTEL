@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
echo === to 0 --nobias (full b0, no bias read) ===
m8proto.exe --to 0 --nobias
echo EXIT=%errorlevel%
