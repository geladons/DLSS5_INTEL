@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
m8proto.exe --to 4 --nots --split 4 --dispdbg > out\_spd.txt 2>&1
echo exit=%errorlevel%
