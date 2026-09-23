@echo off
taskkill /F /IM vkcube.exe >nul 2>&1
set DISABLE_NR_LAYER=1
start "" cmd /c ""C:\VulkanSDK\1.4.357.0\Bin\vkcube.exe" > C:\Users\AI\Desktop\DLSS5_INTEL\work\_m11\vkcube_nolayer.log 2>&1"
