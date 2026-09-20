@echo off
setlocal
rem M8B-LIVE run wrapper: appends a run (args passed through %*) to
rem docs\m8b-live.log. Run from anywhere; exe runs in its Release dir
rem (shaders live next to the exe).

set "REPO=C:\Users\AI\Desktop\DLSS5_INTEL"
set "BUILD=%REPO%\dlss5\m8b-live\build"
set "LOG=%REPO%\docs\m8b-live.log"

echo. >> "%LOG%"
echo === M8B-LIVE run: m8blive.exe %* >> "%LOG%"
echo started %DATE% %TIME% >> "%LOG%"
pushd "%BUILD%\Release"
".\m8blive.exe" %* >> "%LOG%" 2>&1
set FINAL=%errorlevel%
popd
echo exit code: %FINAL% (ended %DATE% %TIME%) >> "%LOG%"
exit /b %FINAL%
