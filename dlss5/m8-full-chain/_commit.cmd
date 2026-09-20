@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL
del dlss5\m8-full-chain\_fix.py 2>nul
del dlss5\m8-full-chain\_runswz.py 2>nul
"C:\Program Files\Git\cmd\git.exe" add docs/m8-full-chain.md docs/m8-golden.log -f dlss5/m8-full-chain/golden.py
"C:\Program Files\Git\cmd\git.exe" status --short
"C:\Program Files\Git\cmd\git.exe" commit -m "m8a: full-chain golden validation"
"C:\Program Files\Git\cmd\git.exe" log --oneline -1
