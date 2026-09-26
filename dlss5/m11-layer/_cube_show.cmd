@echo off
set "REPO=%~dp0\..\.."
taskkill /F /IM vkcube.exe >nul 2>&1
set NR_LAYER_LIVE=1
start "" cmd /c ""C:\VulkanSDK\1.4.357.0\Bin\vkcube.exe" > %REPO%\work\_m11\vkcube_rt.log 2>&1"
