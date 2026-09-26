@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
for %%S in (4 8) do (
  for /l %%R in (1,1,2) do (
    m8proto.exe --to 4 --nots --split %%S > out\_sp%%S_%%R.txt 2>&1
    echo split %%S run %%R exit=%errorlevel%
    findstr /c:"VK error" /c:"GPU-side complete" out\_sp%%S_%%R.txt
  )
)
