@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
for /l %%R in (1,1,3) do (
  m8proto.exe --to 0 --maxdisp 11 > out\_s11_%%R.txt 2>&1
  echo m11 run %%R exit=%errorlevel%
  m8proto.exe --to 0 --maxdisp 12 > out\_s12_%%R.txt 2>&1
  echo m12 run %%R exit=%errorlevel%
  m8proto.exe --to 0 --maxdisp 12 --nobias > out\_s12nb_%%R.txt 2>&1
  echo m12nb run %%R exit=%errorlevel%
)
