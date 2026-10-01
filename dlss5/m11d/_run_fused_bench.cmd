@echo off
setlocal
cd /d "%~dp0build\Release"

echo ======================================================= > "%~dp0_fused_bench.log"
echo 1. Benchmarking Unfused W1+W2 (1920x1088, 5 frames) >> "%~dp0_fused_bench.log"
echo ======================================================= >> "%~dp0_fused_bench.log"
set "D5C_NO_INT8="
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_FUSION=0"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 1920x1088 --prof "out\bench_unfused_1080p.csv" >> "%~dp0_fused_bench.log" 2>&1

echo ======================================================= >> "%~dp0_fused_bench.log"
echo 2. Benchmarking Fused W1+W2 (1920x1088, 5 frames) >> "%~dp0_fused_bench.log"
echo ======================================================= >> "%~dp0_fused_bench.log"
set "D5C_NO_INT8="
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_FUSION=1"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 1920x1088 --prof "out\bench_fused_1080p.csv" >> "%~dp0_fused_bench.log" 2>&1

echo ======================================================= >> "%~dp0_fused_bench.log"
echo 3. Benchmarking Unfused W1+W2 (960x576, 10 frames) >> "%~dp0_fused_bench.log"
echo ======================================================= >> "%~dp0_fused_bench.log"
set "D5C_NO_INT8="
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_FUSION=0"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 10 960x576 --prof "out\bench_unfused_960.csv" >> "%~dp0_fused_bench.log" 2>&1

echo ======================================================= >> "%~dp0_fused_bench.log"
echo 4. Benchmarking Fused W1+W2 (960x576, 10 frames) >> "%~dp0_fused_bench.log"
echo ======================================================= >> "%~dp0_fused_bench.log"
set "D5C_NO_INT8="
set "D5C_INT8_W1=70"
set "D5C_INT8_W2=70"
set "D5C_INT8_FUSION=1"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 10 960x576 --prof "out\bench_fused_960.csv" >> "%~dp0_fused_bench.log" 2>&1

echo EXITCODE=%ERRORLEVEL% >> "%~dp0_fused_bench.log"
