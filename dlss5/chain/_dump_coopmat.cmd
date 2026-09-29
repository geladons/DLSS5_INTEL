@echo off
REM Dump all cooperative matrix configs from vulkaninfo
C:\VulkanSDK\Bin\vulkaninfo.exe --summary 2>nul | findstr /i "coopmat coop cooperative"
echo ---
C:\VulkanSDK\Bin\vulkaninfo.exe 2>nul | findstr /i /c:"cooperative" /c:"MSize" /c:"NSize" /c:"KSize" /c:"AType" /c:"BType" /c:"CType" /c:"ResultType" /c:"saturating"
