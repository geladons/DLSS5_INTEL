@echo off
rem Roundtrip test: stub daemon (green tint) + vkcube, every present processed.
start "" /min cmd /c "python C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11-layer\daemon_stub.py > C:\Users\AI\Desktop\DLSS5_INTEL\work\_m11\stub.log 2>&1"
timeout /t 2 /nobreak >nul
set NR_LAYER_LIVE=1
start "" /min cmd /c ""C:\VulkanSDK\1.4.357.0\Bin\vkcube.exe" > C:\Users\AI\Desktop\DLSS5_INTEL\work\_m11\vkcube_rt.log 2>&1"
