@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
for /l %%R in (1,1,5) do (
  m8proto.exe --to 0 > out\_v_%%R.txt 2>&1
  copy out\dbg_b0raw.bin out\v_%%R.bin >nul
)
cd ..\..
for /l %%R in (1,1,5) do (
  echo --- run %%R ---
  C:\Users\AI\AppData\Local\Programs\Python\Python312\python.exe diffrow.py build\Release\out\v_1.bin build\Release\out\v_%%R.bin
)
