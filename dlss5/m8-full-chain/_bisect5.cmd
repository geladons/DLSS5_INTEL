@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
for %%N in (7 8 9 10 11 12) do (
  echo === maxdisp %%N ===
  m8proto.exe --to 0 --maxdisp %%N > out\_bd_%%N.txt 2>&1
  echo EXIT=%errorlevel%
  findstr /c:"dispatches executed" /c:"VK error" /c:"GPU-side complete" out\_bd_%%N.txt
)
