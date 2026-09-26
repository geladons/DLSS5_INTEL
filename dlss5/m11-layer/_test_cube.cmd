@echo off
set "REPO=%~dp0\..\.."
rem vkcube + layer, capture the 100th present to a file (no daemon needed).
set ENABLE_NR_LAYER=1
set NR_LAYER_EVERY=100
set NR_LAYER_CAPTURE=%REPO%\work\_m11\cube_capture.bin
if not exist %REPO%\work\_m11 mkdir %REPO%\work\_m11
start "" /min "C:\VulkanSDK\1.4.357.0\Bin\vkcube.exe"
