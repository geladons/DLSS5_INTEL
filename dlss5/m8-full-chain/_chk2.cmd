@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
for %%F in (out\_sp2_1.txt out\_sp2_2.txt out\_sp2_3.txt) do (
  echo === %%F ===
  findstr /c:"VK error" /c:"GPU-side complete" %%F
  powershell -NoProfile -Command "$ls = Get-Content %%F; $ls | Select-String -Pattern '\[disp' | Select-Object -Last 3 | ForEach-Object { $_.Line }"
  powershell -NoProfile -Command "$ls = Get-Content %%F; ($ls | Select-String -Pattern 'dispatches executed').Line"
)
