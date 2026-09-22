@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m9-unet
call build.cmd > _build.log 2>&1
exit /b %errorlevel%
