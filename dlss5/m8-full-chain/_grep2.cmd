@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain
findstr /n /c:"makePipeline" main.cpp
echo ---
findstr /n /c:"pushConstant" /c:"push constant" /c:"PushConstantRange" /c:"range" main.cpp | findstr /v /c:"vecOff" | findstr /v /c:"arrange"
