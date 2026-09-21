@echo off
cd /d C:\Users\<owner>\Desktop\DLSS5_INTEL
powershell -NoProfile -Command "Get-Content docs\m8b-live.log -Tail 60"
