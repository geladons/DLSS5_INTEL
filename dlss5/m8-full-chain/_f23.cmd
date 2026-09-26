@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain
call _rb.cmd
if errorlevel 1 exit /b 1
cd build\Release
echo === block 2 (maxdisp 46) ===
m8proto.exe --to 3 --maxdisp 46 --dispdbg > out\_f2.txt 2>&1
cd ..\..
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe cmp_blk.py %REPO%\work\mlxw\dlssnr-logical.safetensors build\Release\out 2
cd build\Release
echo === block 3 (maxdisp 60) ===
m8proto.exe --to 3 --maxdisp 60 --dispdbg > out\_f3.txt 2>&1
cd ..\..
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe cmp_blk.py %REPO%\work\mlxw\dlssnr-logical.safetensors build\Release\out 3
