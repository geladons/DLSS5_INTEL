@echo off
setlocal
cd /d "%~dp0build\Release"

echo ======================================================= > "%~dp0_w2_bench.log"
echo 1. Benchmarking FP16 Baseline (1920x1088, 5 frames) >> "%~dp0_w2_bench.log"
echo ======================================================= >> "%~dp0_w2_bench.log"
set "D5C_NO_INT8=1"
set "D5C_INT8_W1="
set "D5C_INT8_W2="
set "D5C_INT8_PILOT="
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 1920x1088 --prof "out\bench_fp16_1080p.csv" >> "%~dp0_w2_bench.log" 2>&1

echo ======================================================= >> "%~dp0_w2_bench.log"
echo 2. Benchmarking INT8 Weight2 Only (1920x1088, 5 frames) >> "%~dp0_w2_bench.log"
echo ======================================================= >> "%~dp0_w2_bench.log"
set "D5C_NO_INT8="
set "D5C_INT8_W1=-1"
set "D5C_INT8_W2=70"
set "D5C_INT8_PILOT="
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 1920x1088 --prof "out\bench_w2_1080p.csv" >> "%~dp0_w2_bench.log" 2>&1

echo ======================================================= >> "%~dp0_w2_bench.log"
echo 3. Benchmarking INT8 W1+W2 (1920x1088, 5 frames) >> "%~dp0_w2_bench.log"
echo ======================================================= >> "%~dp0_w2_bench.log"
set "D5C_NO_INT8="
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_PILOT="
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 1920x1088 --prof "out\bench_w1w2_1080p.csv" >> "%~dp0_w2_bench.log" 2>&1

echo ======================================================= >> "%~dp0_w2_bench.log"
echo 4. Benchmarking FP16 Baseline (960x576, 10 frames) >> "%~dp0_w2_bench.log"
echo ======================================================= >> "%~dp0_w2_bench.log"
set "D5C_NO_INT8=1"
set "D5C_INT8_W1="
set "D5C_INT8_W2="
set "D5C_INT8_PILOT="
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 10 960x576 --prof "out\bench_fp16_960.csv" >> "%~dp0_w2_bench.log" 2>&1

echo ======================================================= >> "%~dp0_w2_bench.log"
echo 5. Benchmarking INT8 Weight2 Only (960x576, 10 frames) >> "%~dp0_w2_bench.log"
echo ======================================================= >> "%~dp0_w2_bench.log"
set "D5C_NO_INT8="
set "D5C_INT8_W1=-1"
set "D5C_INT8_W2=70"
set "D5C_INT8_PILOT="
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 10 960x576 --prof "out\bench_w2_960.csv" >> "%~dp0_w2_bench.log" 2>&1

echo ======================================================= >> "%~dp0_w2_bench.log"
echo 6. Benchmarking INT8 W1+W2 (960x576, 10 frames) >> "%~dp0_w2_bench.log"
echo ======================================================= >> "%~dp0_w2_bench.log"
set "D5C_NO_INT8="
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_PILOT="
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 10 960x576 --prof "out\bench_w1w2_960.csv" >> "%~dp0_w2_bench.log" 2>&1

echo EXITCODE=%ERRORLEVEL% >> "%~dp0_w2_bench.log"
