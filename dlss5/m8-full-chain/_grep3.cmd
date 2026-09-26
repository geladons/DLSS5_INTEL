@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain
findstr /n /c:"fullBarrier" main.cpp
echo ---
findstr /n /c:"VkMemoryBarrier" /c:"vkCmdPipelineBarrier" main.cpp
