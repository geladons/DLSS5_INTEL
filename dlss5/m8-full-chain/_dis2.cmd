@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain
findstr /n "OpConstant" _ew.spvasm
echo === literals ===
findstr /n "0x0000000" _ew.spvasm | more +0
