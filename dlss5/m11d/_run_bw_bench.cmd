@echo off
setlocal
cd /d "%~dp0build\Release"

set "WEIGHTS=c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors"

if exist "%~dp0_bench_bw_done.log" del "%~dp0_bench_bw_done.log"

echo === Running bench 960x576 === > "%~dp0_bench_bw_960.log"
m11d.exe --weights "%WEIGHTS%" --bench 5 960x576 --prof "%~dp0bench_bw_960.csv" >> "%~dp0_bench_bw_960.log" 2>&1
echo EXITCODE_960=%ERRORLEVEL% >> "%~dp0_bench_bw_960.log"

echo === Running bench 1920x1088 === > "%~dp0_bench_bw_1080p.log"
m11d.exe --weights "%WEIGHTS%" --bench 5 1920x1088 --prof "%~dp0bench_bw_1080p.csv" >> "%~dp0_bench_bw_1080p.log" 2>&1
echo EXITCODE_1080P=%ERRORLEVEL% >> "%~dp0_bench_bw_1080p.log"

if exist "%~dp0bench_bw_960.csv" copy /y "%~dp0bench_bw_960.csv" "%~dp0build\Release\bench_bw_960.csv" >nul
if exist "%~dp0bench_bw_1080p.csv" copy /y "%~dp0bench_bw_1080p.csv" "%~dp0build\Release\bench_bw_1080p.csv" >nul

echo BENCH_DONE >> "%~dp0_bench_bw_done.log"
