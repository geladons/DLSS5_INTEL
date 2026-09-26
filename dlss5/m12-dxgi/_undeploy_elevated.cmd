@echo off
set "REPO=%~dp0\..\.."
rem Elevated one-shot: remove the m12 proxy from the Enhanced game folder.
if exist "D:\Grand Theft Auto V Enhanced\dxgi.dll" del "D:\Grand Theft Auto V Enhanced\dxgi.dll"
echo exit=%errorlevel% > %REPO%\dlss5\m12-dxgi\undeploy_result.txt
