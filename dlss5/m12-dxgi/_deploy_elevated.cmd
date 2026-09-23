@echo off
rem Elevated one-shot: copy the m12 proxy next to GTA5_Enhanced.exe.
copy /y "C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m12-dxgi\build\Release\m12_dxgi.dll" "D:\Grand Theft Auto V Enhanced\dxgi.dll"
echo exit=%errorlevel% > C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m12-dxgi\deploy_result.txt
