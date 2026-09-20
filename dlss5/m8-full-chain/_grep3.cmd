@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain
findstr /n /c:"fullBarrier" main.cpp
echo ---
findstr /n /c:"VkMemoryBarrier" /c:"vkCmdPipelineBarrier" main.cpp
