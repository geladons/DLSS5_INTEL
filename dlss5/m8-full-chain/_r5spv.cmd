@echo off
setlocal
set "REPO=%~dp0\..\.."
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"
cd /d %REPO%\dlss5\m8-full-chain\build\Release
glslangValidator.exe -H softmax.spv > out\_r5_smax_h.txt 2>&1
echo exit=%errorlevel%
findstr /c:"Push" /c:"Block" /c:"Member" /c:"OpType" out\_r5_smax_h.txt | more +0
