@echo off
reg add "HKCU\Software\Khronos\Vulkan\ImplicitLayers" /v "C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11-layer\build\Release\VkLayer_dlssnr_win.json" /t REG_DWORD /d 0 /f
reg query "HKCU\Software\Khronos\Vulkan\ImplicitLayers"
