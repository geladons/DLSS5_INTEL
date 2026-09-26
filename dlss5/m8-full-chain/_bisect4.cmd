@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
for %%N in (1 2 3 4 5 6) do (
  echo === maxdisp %%N ===
  m8proto.exe --to 0 --maxdisp %%N > out\_bd_%%N.txt 2>&1
  echo EXIT=%errorlevel%
  findstr /c:"dispatches executed" /c:"VK error" /c:"GPU-side complete" out\_bd_%%N.txt
)
