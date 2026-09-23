@echo off
rem vkcube + layer, capture the 100th present to a file (no daemon needed).
set ENABLE_NR_LAYER=1
set NR_LAYER_EVERY=100
set NR_LAYER_CAPTURE=C:\Users\AI\Desktop\DLSS5_INTEL\work\_m11\cube_capture.bin
if not exist C:\Users\AI\Desktop\DLSS5_INTEL\work\_m11 mkdir C:\Users\AI\Desktop\DLSS5_INTEL\work\_m11
start "" /min "C:\VulkanSDK\1.4.357.0\Bin\vkcube.exe"
