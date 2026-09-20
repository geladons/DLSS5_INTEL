@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
m8proto.exe --to 3 --maxdisp 34 --dispdbg > out\_r34.txt 2>&1
copy out\dbg_b2g4mid.bin out\g4_34.bin >nul
m8proto.exe --to 3 --maxdisp 35 --dispdbg > out\_r35.txt 2>&1
copy out\dbg_b2g4mid.bin out\g4_35.bin >nul
cd ..\..
C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe diffrow.py build\Release\out\g4_34.bin build\Release\out\g4_35.bin
