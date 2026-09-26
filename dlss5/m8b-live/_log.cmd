@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%
powershell -NoProfile -Command "Get-Content docs\m8b-live.log -Tail 60"
