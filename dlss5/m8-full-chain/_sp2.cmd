@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
for /l %%R in (1,1,3) do (
  m8proto.exe --to 4 --nots --split 2 --dispdbg > out\_sp2_%%R.txt 2>&1
  echo run %%R exit=%errorlevel%
)
powershell -NoProfile -Command "Get-Content out\_sp2_1.txt -Tail 6"
echo === run2 ===
powershell -NoProfile -Command "Get-Content out\_sp2_2.txt -Tail 6"
echo === run3 ===
powershell -NoProfile -Command "Get-Content out\_sp2_3.txt -Tail 6"
