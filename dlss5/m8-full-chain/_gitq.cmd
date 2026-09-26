@echo off
set "REPO=%~dp0\..\.."
"C:\Program Files\Git\cmd\git.exe" -C %REPO% log --oneline -3
echo === status main.cpp ===
"C:\Program Files\Git\cmd\git.exe" -C %REPO% status --short dlss5/m8-full-chain/main.cpp
echo === diff stat HEAD ===
"C:\Program Files\Git\cmd\git.exe" -C %REPO% diff --stat HEAD -- dlss5/m8-full-chain/main.cpp
