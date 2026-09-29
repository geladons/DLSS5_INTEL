@echo off
setlocal
cd /d "%~dp0build\Release"

echo ======================================================= > "%~dp0_attn_bench.log"
echo 1. Benchmarking Baseline Fused W1+W2 (960x576, 10 frames) >> "%~dp0_attn_bench.log"
echo ======================================================= >> "%~dp0_attn_bench.log"
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_FUSION=1"
set "D5C_INT8_ATTN=-1"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 10 960x576 --prof "out\bench_fused_baseline_960.csv" >> "%~dp0_attn_bench.log" 2>&1

echo ======================================================= >> "%~dp0_attn_bench.log"
echo 2. Benchmarking INT8 Attention (960x576, 10 frames) >> "%~dp0_attn_bench.log"
echo ======================================================= >> "%~dp0_attn_bench.log"
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_FUSION=1"
set "D5C_INT8_ATTN=70"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 10 960x576 --prof "out\bench_attn_960.csv" >> "%~dp0_attn_bench.log" 2>&1

echo ======================================================= >> "%~dp0_attn_bench.log"
echo 3. Benchmarking Baseline Fused W1+W2 (1920x1088, 5 frames) >> "%~dp0_attn_bench.log"
echo ======================================================= >> "%~dp0_attn_bench.log"
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_FUSION=1"
set "D5C_INT8_ATTN=-1"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 1920x1088 --prof "out\bench_fused_baseline_1080p.csv" >> "%~dp0_attn_bench.log" 2>&1

echo ======================================================= >> "%~dp0_attn_bench.log"
echo 4. Benchmarking INT8 Attention (1920x1088, 5 frames) >> "%~dp0_attn_bench.log"
echo ======================================================= >> "%~dp0_attn_bench.log"
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_FUSION=1"
set "D5C_INT8_ATTN=70"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 1920x1088 --prof "out\bench_attn_1080p.csv" >> "%~dp0_attn_bench.log" 2>&1

echo === Done benchmarking === >> "%~dp0_attn_bench.log"
