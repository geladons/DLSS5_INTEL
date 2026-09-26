@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%
"C:\Program Files\Git\cmd\git.exe" log --oneline -3
echo ---DIFF STAT---
"C:\Program Files\Git\cmd\git.exe" diff --stat HEAD -- dlss5/m8-full-chain
echo ---DIFF NAME-ONLY ALL---
"C:\Program Files\Git\cmd\git.exe" diff --name-only HEAD
