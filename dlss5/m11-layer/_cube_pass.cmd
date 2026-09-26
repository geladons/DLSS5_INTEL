@echo off
set "REPO=%~dp0\..\.."
taskkill /F /IM vkcube.exe >nul 2>&1
rem passthrough: layer loaded, but no LIVE/TRIGGER/CAPTURE -> presents untouched
start "" cmd /c ""C:\VulkanSDK\1.4.357.0\Bin\vkcube.exe" > %REPO%\work\_m11\vkcube_pass.log 2>&1"
