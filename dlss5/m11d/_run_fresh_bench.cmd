@echo off
cd /d "%~dp0build\Release"

m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 960x576 --prof bench_fresh_960.csv > _bench_fresh_960.log 2>&1
echo EXITCODE_960=%ERRORLEVEL% >> _bench_fresh_960.log

m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 3 1920x1088 --prof bench_fresh_1080p.csv > _bench_fresh_1080p.log 2>&1
echo EXITCODE_1080P=%ERRORLEVEL% >> _bench_fresh_1080p.log
echo BENCH_DONE >> _bench_fresh_1080p.log

copy /y _bench_fresh_960.log "%~dp0_bench_fresh_960.log" > nul
copy /y _bench_fresh_1080p.log "%~dp0_bench_fresh_1080p.log" > nul
copy /y bench_fresh_960.csv "%~dp0bench_fresh_960.csv" > nul
copy /y bench_fresh_1080p.csv "%~dp0bench_fresh_1080p.csv" > nul
echo BENCH_DONE >> "%~dp0_bench_fresh_done.log"
