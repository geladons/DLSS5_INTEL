@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
powershell -NoProfile -Command "Get-Content out\_spd.txt -Tail 25"
