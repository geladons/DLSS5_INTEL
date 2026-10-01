@echo off
setlocal
cd /d "%~dp0build\Release"

echo ======================================================= > "%~dp0_int8_bench.log"
echo 1. Benchmarking FP16 Baseline (960x576, 10 frames) >> "%~dp0_int8_bench.log"
echo ======================================================= >> "%~dp0_int8_bench.log"
set "D5C_NO_INT8=1"
set "D5C_INT8_PILOT="
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 10 960x576 --prof "out\bench_fp16_960.csv" >> "%~dp0_int8_bench.log" 2>&1

echo ======================================================= >> "%~dp0_int8_bench.log"
echo 2. Benchmarking INT8 Pilot70 (960x576, 10 frames) >> "%~dp0_int8_bench.log"
echo ======================================================= >> "%~dp0_int8_bench.log"
set "D5C_NO_INT8="
set "D5C_INT8_PILOT=70"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 10 960x576 --prof "out\bench_int8_960.csv" >> "%~dp0_int8_bench.log" 2>&1

echo ======================================================= >> "%~dp0_int8_bench.log"
echo 3. Benchmarking FP16 Baseline (1920x1088, 5 frames) >> "%~dp0_int8_bench.log"
echo ======================================================= >> "%~dp0_int8_bench.log"
set "D5C_NO_INT8=1"
set "D5C_INT8_PILOT="
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 1920x1088 --prof "out\bench_fp16_1080p.csv" >> "%~dp0_int8_bench.log" 2>&1

echo ======================================================= >> "%~dp0_int8_bench.log"
echo 4. Benchmarking INT8 Pilot70 (1920x1088, 5 frames) >> "%~dp0_int8_bench.log"
echo ======================================================= >> "%~dp0_int8_bench.log"
set "D5C_NO_INT8="
set "D5C_INT8_PILOT=70"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 1920x1088 --prof "out\bench_int8_1080p.csv" >> "%~dp0_int8_bench.log" 2>&1

echo EXITCODE=%ERRORLEVEL% >> "%~dp0_int8_bench.log"
