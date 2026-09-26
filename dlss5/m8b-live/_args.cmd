@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8b-live
findstr /n /c:"--frames" /c:"--novideo" /c:"--verify" /c:"--maxdelta" /c:"--region" /c:"--swapstorage" /c:"--scale" main.cpp
