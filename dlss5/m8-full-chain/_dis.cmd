@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain
"C:\VulkanSDK\1.4.357.0\Bin\spirv-dis.exe" build\Release\elementwise.spv > _ew.spvasm
findstr /n "OpConstant _ew.spvasm" | findstr /n "."
