@echo off
cd /d "%~dp0"
call build.cmd > _build_coopmat.log 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED - see _build_coopmat.log
    exit /b 1
)
echo BUILD OK
build\Release\m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 3 960x576 --prof "build\Release\bench_fresh.csv" > _bench_fresh.log 2>&1
echo BENCH EXITCODE=%ERRORLEVEL%
type _bench_fresh.log | findstr /i "coopmat cooperative AVAILABLE OK TF bench chain"
