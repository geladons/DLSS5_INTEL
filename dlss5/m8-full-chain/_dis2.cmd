@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain
findstr /n "OpConstant" _ew.spvasm
echo === literals ===
findstr /n "0x0000000" _ew.spvasm | more +0
