@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain
call build_m8.cmd --no-run
cd build\Release
set M8_DEBUG_BIAS=1
m8proto.exe --to 0 --dispdbg > out\_r5_dbg5.txt 2>&1
echo exit=%errorlevel%
findstr /c:"bias-swz" out\_r5_dbg5.txt | more +0
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe _r5cmp7.py
