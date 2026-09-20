@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
echo === stem --to 4 ===
m8proto.exe --to 4 > out\_r5_stem.txt 2>&1
echo exit=%errorlevel%
findstr /c:"VK error" /c:"GPU-side" /c:"family" /c:"dispatches" /c:"total" out\_r5_stem.txt
echo === full b0 --to 0 (was crashing) ===
m8proto.exe --to 0 > out\_r5_b0.txt 2>&1
echo exit=%errorlevel%
findstr /c:"VK error" /c:"GPU-side" out\_r5_b0.txt
