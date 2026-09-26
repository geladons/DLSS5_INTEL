@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
for %%N in (5 11 16) do (
  echo === maxdisp %%N ===
  m8proto.exe --to 0 --maxdisp %%N | findstr /c:"dispatches executed" /c:"VK error" /c:"GPU-side complete"
  echo EXIT=%errorlevel%
)
