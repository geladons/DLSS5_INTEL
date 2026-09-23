@echo off
setlocal
rem Remove the m12 proxy from the game folder (BattleEye fallback path).

set "GAME=D:\Grand Theft Auto V Enhanced"
if exist "%GAME%\dxgi.dll" del "%GAME%\dxgi.dll"
echo removed %GAME%\dxgi.dll (restart the game)
exit /b 0
