@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain
call build_m8.cmd --no-run
cd build\Release
m8proto.exe --to 0 --dispdbg > out\_r5_dbg4.txt 2>&1
echo exit=%errorlevel%
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe _r5cmp7.py
