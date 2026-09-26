@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
for %%M in (1 2 3 4) do (
  m8proto.exe --to 0 --nots --split 1 --pvmode %%M > out\_pvm%%M.txt 2>&1
  echo pvmode %%M exit=%errorlevel%
  findstr /c:"VK error" /c:"GPU-side complete" out\_pvm%%M.txt
)
