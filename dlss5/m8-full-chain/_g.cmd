@echo off
set "REPO=%~dp0\..\.."
"C:\Program Files\Git\cmd\git.exe" -C %REPO% %*
