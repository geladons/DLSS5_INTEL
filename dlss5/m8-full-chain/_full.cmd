@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
for /l %%R in (1,1,4) do (
  echo === full chain run %%R ===
  m8proto.exe > out\_full_%%R.txt 2>&1
  echo run %%R exit=%errorlevel%
  findstr /c:"steady-state" /c:"VK error" /c:"GPU-side complete" /c:"chain total" out\_full_%%R.txt
  if not errorlevel 1 goto :done
)
:done
