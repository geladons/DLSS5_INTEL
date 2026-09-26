@echo off
setlocal
rem M1 DDA capture: configure, build, run. Single orchestration point (cmd
rem batch — immune to the exec wrapper's PowerShell $-mangling). All output
rem (build + run console) lands in docs\m1-capture.log.

set "REPO=%~dp0\..\.."
set "SRC=%REPO%\dlss5\m1-frame-capture"
set "BUILD=%SRC%\build"
set "LOG=%REPO%\docs\m1-capture.log"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"

rem run args: frames snapshot_interval timeout_ms outdir
set FRAMES=300
set SNAPINT=60
set TIMEOUTMS=500
set OUTDIR=out
if not "%~1"=="" set FRAMES=%~1
if not "%~2"=="" set SNAPINT=%~2
if not "%~3"=="" set TIMEOUTMS=%~3
if not "%~4"=="" set OUTDIR=%~4

echo === M1 DXGI Desktop Duplication build+run log ===> "%LOG%"
echo started %DATE% %TIME% >> "%LOG%"
echo args: frames=%FRAMES% snapint=%SNAPINT% timeout=%TIMEOUTMS% outdir=%OUTDIR% >> "%LOG%"
"%CMAKE%" --version >> "%LOG%" 2>&1

echo ^>^> cmake configure >> "%LOG%"
"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64 >> "%LOG%" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )

echo ^>^> cmake build >> "%LOG%"
"%CMAKE%" --build "%BUILD%" --config Release >> "%LOG%" 2>&1
if errorlevel 1 ( set FINAL=1 & goto :done )

if not exist "%BUILD%\Release\m1dda.exe" ( set FINAL=1 & goto :done )

echo ^>^> run capture >> "%LOG%"
pushd "%BUILD%\Release"
".\m1dda.exe" %FRAMES% %SNAPINT% %TIMEOUTMS% %OUTDIR% >> "%LOG%" 2>&1
set FINAL=%errorlevel%
popd
echo m1dda exit code: %FINAL% >> "%LOG%"

:done
echo. >> "%LOG%"
echo === final exit code: %FINAL% (ended %DATE% %TIME%) === >> "%LOG%"
type "%LOG%"
exit /b %FINAL%
