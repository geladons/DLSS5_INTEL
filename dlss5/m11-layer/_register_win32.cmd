@echo off
set "REPO=%~dp0\..\.."
rem Register the 32-bit m11 layer manifest. Run as a FILE (inline quoting
rem through git bash mangles quotes: a bogus 'ImplicitLayers"' key with
rem quote-prefixed value names resulted - see _enum_layers_key.ps1 history).
reg delete "HKCU\Software\Khronos\Vulkan\ImplicitLayers""" /f
reg add "HKCU\Software\Khronos\Vulkan\ImplicitLayers" /v "%REPO%\dlss5\m11-layer\x86\VkLayer_dlssnr_win32.json" /t REG_DWORD /d 0 /f
reg query "HKCU\Software\Khronos\Vulkan\ImplicitLayers"
