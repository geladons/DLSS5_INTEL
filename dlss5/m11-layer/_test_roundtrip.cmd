@echo off
set "REPO=%~dp0\..\.."
rem Roundtrip test: stub daemon (green tint) + vkcube, every present processed.
start "" /min cmd /c "python %REPO%\dlss5\m11-layer\daemon_stub.py > %REPO%\work\_m11\stub.log 2>&1"
timeout /t 2 /nobreak >nul
set NR_LAYER_LIVE=1
start "" /min cmd /c ""C:\VulkanSDK\1.4.357.0\Bin\vkcube.exe" > %REPO%\work\_m11\vkcube_rt.log 2>&1"
