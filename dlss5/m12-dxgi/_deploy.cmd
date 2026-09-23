@echo off
setlocal
rem Deploy the m12 DXGI proxy next to GTA5 Enhanced (DX12 + BattleEye).
rem The game must be RESTARTED to pick the proxy up. If BattleEye blocks it
rem and the game stops starting, run _undeploy.cmd and restart again.

set "GAME=D:\Grand Theft Auto V Enhanced"
set "SRC=C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m12-dxgi"
if not exist "%GAME%\GTA5_Enhanced.exe" (
    echo game not found at %GAME%
    exit /b 1
)
copy /y "%SRC%\build\Release\m12_dxgi.dll" "%GAME%\dxgi.dll" >nul
echo deployed: %GAME%\dxgi.dll
exit /b 0
