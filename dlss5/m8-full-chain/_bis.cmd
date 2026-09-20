@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
for %%M in (17 20 22 24 28 32) do (
  m8proto.exe --to 3 --maxdisp %%M > out\_b_%%M.txt 2>&1
  cd ..\..
  echo --- maxdisp %%M ---
  C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe chkraw.py C:\Users\AI\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors build\Release\out
  cd build\Release
)
