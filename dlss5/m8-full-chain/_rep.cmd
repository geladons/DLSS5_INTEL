@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
m8proto.exe --to 0 > out\_rep1.txt 2>&1
copy out\dbg_b0raw.bin out\rep1.bin >nul
m8proto.exe --to 0 > out\_rep2.txt 2>&1
copy out\dbg_b0raw.bin out\rep2.bin >nul
cd ..\..
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe diffrow.py build\Release\out\rep1.bin build\Release\out\rep2.bin
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe chkraw2.py
