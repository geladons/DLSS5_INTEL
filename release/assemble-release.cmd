@echo off
rem assemble-release.cmd - build the public demo bundle from the local
rem production folder. ASCII-only by policy (host launcher quirk).
rem
rem Usage:  release\assemble-release.cmd [source-dir] [target-dir]
rem   source  default: %USERPROFILE%\Desktop\production
rem   target  default: <repo>\release\out\DLSS5-Demo-Bundle
rem
rem The bundle deliberately EXCLUDES:
rem   - weights (*.safetensors) - NVIDIA proprietary, not distributed
rem   - logs, before/after sources, __pycache__, runtime frame dumps (out\)
rem A placeholder README is written into weights\ telling the user what file
rem to obtain and where the format is documented.

setlocal
set SRC=%~1
if "%SRC%"=="" set SRC=%USERPROFILE%\Desktop\production
set DST=%~2
if "%DST%"=="" set DST=%~dp0out\DLSS5-Demo-Bundle

if not exist "%SRC%\m13" (
  echo [ERROR] production folder not found: %SRC%
  echo         pass it as the first argument.
  exit /b 1
)

if exist "%DST%" rmdir /s /q "%DST%"
mkdir "%DST%" 2>nul

echo [1/3] Copying bundle files ...
robocopy "%SRC%" "%DST%" /E /NFL /NDL /NJH /NJS ^
  /XD "%SRC%\beforeafter" "%SRC%\logs" "%SRC%\m13\__pycache__" ^
      "%SRC%\runtime\m8b\out" "%SRC%\runtime\m11d\out" ^
  /XF *.safetensors
if %ERRORLEVEL% GEQ 8 (
  echo [ERROR] robocopy failed with code %ERRORLEVEL%
  exit /b 1
)

echo [2/3] Writing weights placeholder ...
if not exist "%DST%\weights" mkdir "%DST%\weights"
> "%DST%\weights\README.txt" echo DLSS 5 neural-rendering weights are NOT included.
>>"%DST%\weights\README.txt" echo.
>>"%DST%\weights\README.txt" echo Place exactly one file here:
>>"%DST%\weights\README.txt" echo.
>>"%DST%\weights\README.txt" echo   dlssnr-logical.safetensors   (~291.5 MB, 649 tensors, 71 block groups)
>>"%DST%\weights\README.txt" echo.
>>"%DST%\weights\README.txt" echo Full format spec: docs/weights-format.md in the GitHub repository.
>>"%DST%\weights\README.txt" echo SHA-256: 203b0af3be94078cfd17a71adc4628cffd960a4d435e4820fe994ddd9493a6a5

echo [3/3] Done. Bundle at:
echo   %DST%
echo.
echo Next: zip the folder and attach it to a GitHub Release.
endlocal
exit /b 0
