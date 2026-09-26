@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8b-live
findstr /n /c:"verify" main.cpp
