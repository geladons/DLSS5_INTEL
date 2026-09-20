@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
for /l %%R in (1,1,3) do (
  m8proto.exe --to 4 > out\_stem_%%R.txt 2>&1
  echo stem run %%R exit=%errorlevel%
  findstr /c:"VK error" /c:"GPU-side complete" out\_stem_%%R.txt
)
