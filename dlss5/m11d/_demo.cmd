@echo off
rem M11D live demo: real DLSSNR daemon + vkcube through the Vulkan layer.
rem m11d console stays VISIBLE (per-frame timings). Close both windows after.
taskkill /F /IM vkcube.exe >nul 2>&1
taskkill /F /IM m11d.exe >nul 2>&1
timeout /t 1 /nobreak >nul
start "m11d" /D "C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11d\build-nmake" cmd /c "m11d.exe > C:\Users\AI\Desktop\DLSS5_INTEL\work\_m11\m11d.log 2>&1"
timeout /t 2 /nobreak >nul
set NR_LAYER_LIVE=1
start "" cmd /c ""C:\VulkanSDK\1.4.357.0\Bin\vkcube.exe" > C:\Users\AI\Desktop\DLSS5_INTEL\work\_m11\vkcube_rt.log 2>&1"
