@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
for /l %%R in (1,1,3) do (
  m8proto.exe --to 4 --nots > out\_nts_%%R.txt 2>&1
  echo nts stem run %%R exit=%errorlevel%
  findstr /c:"VK error" /c:"GPU-side complete" /c:"family stem" out\_nts_%%R.txt
)
