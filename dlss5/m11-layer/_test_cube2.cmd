@echo off
taskkill /F /IM vkcube.exe >nul 2>&1
set ENABLE_NR_LAYER=1
set VK_LOADER_DEBUG=all
set NR_LAYER_EVERY=100
set NR_LAYER_CAPTURE=C:\Users\AI\Desktop\DLSS5_INTEL\work\_m11\cube_capture.bin
start "" /min cmd /c ""C:\VulkanSDK\1.4.357.0\Bin\vkcube.exe" > C:\Users\AI\Desktop\DLSS5_INTEL\work\_m11\vkcube_loader.log 2>&1"
