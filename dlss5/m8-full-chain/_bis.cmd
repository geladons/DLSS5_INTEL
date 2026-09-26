@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain\build\Release
for %%M in (17 20 22 24 28 32) do (
  m8proto.exe --to 3 --maxdisp %%M > out\_b_%%M.txt 2>&1
  cd ..\..
  echo --- maxdisp %%M ---
  %USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe chkraw.py %REPO%\work\mlxw\dlssnr-logical.safetensors build\Release\out
  cd build\Release
)
