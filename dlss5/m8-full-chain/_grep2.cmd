@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain
findstr /n /c:"makePipeline" main.cpp
echo ---
findstr /n /c:"pushConstant" /c:"push constant" /c:"PushConstantRange" /c:"range" main.cpp | findstr /v /c:"vecOff" | findstr /v /c:"arrange"
