@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain
call build_m8.cmd --no-run
cd build\Release
m8proto.exe --to 0 --dispdbg > out\_r5_dbg2.txt 2>&1
echo exit=%errorlevel%
findstr /c:"debug arena" out\_r5_dbg2.txt
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe _r5cmp3.py
