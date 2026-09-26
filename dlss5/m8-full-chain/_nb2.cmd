@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
echo === to 0 --nobias --dispdbg ===
m8proto.exe --to 0 --nobias --dispdbg 2>&1 | findstr /c:"[disp 12]" /c:"[disp 17]" /c:"VK error" /c:"GPU-side complete" /c:"EXIT"
echo PIPE-EXIT
m8proto.exe --to 0 --nobias > out\_nb.txt 2>&1
echo REAL-EXIT=%errorlevel%
findstr /c:"VK error" /c:"GPU-side complete" out\_nb.txt
