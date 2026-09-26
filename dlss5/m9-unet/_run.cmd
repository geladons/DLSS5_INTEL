@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m9-unet\build\Release
start "" /min cmd /c "m9unet.exe --extent 512x320 --features %REPO%\work\_ref_test\golden_features.bin %* > run.log 2>&1"
exit /b 0
