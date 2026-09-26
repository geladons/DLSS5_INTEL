@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
m8proto.exe --to 0 > out\_rep1.txt 2>&1
copy out\dbg_b0raw.bin out\rep1.bin >nul
m8proto.exe --to 0 > out\_rep2.txt 2>&1
copy out\dbg_b0raw.bin out\rep2.bin >nul
cd ..\..
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe diffrow.py build\Release\out\rep1.bin build\Release\out\rep2.bin
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe chkraw2.py
