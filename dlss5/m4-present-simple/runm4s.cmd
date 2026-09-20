@echo off
setlocal
rem M4-SIMPLE run wrapper: appends a run (args passed through %*) to
rem docs\m4-simple.log. Run from anywhere; exe runs in its Release dir
rem (shaders live next to the exe).

set "REPO=C:\Users\AI\Desktop\DLSS5_INTEL"
set "BUILD=%REPO%\dlss5\m4-present-simple\build"
set "LOG=%REPO%\docs\m4-simple.log"

echo. >> "%LOG%"
echo === M4-SIMPLE run: m4simple.exe %* >> "%LOG%"
echo started %DATE% %TIME% >> "%LOG%"
pushd "%BUILD%\Release"
".\m4simple.exe" %* >> "%LOG%" 2>&1
set FINAL=%errorlevel%
popd
echo exit code: %FINAL% (ended %DATE% %TIME%) >> "%LOG%"
exit /b %FINAL%
