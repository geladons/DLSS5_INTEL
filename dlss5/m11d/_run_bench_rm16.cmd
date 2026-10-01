@echo off
cd /d "%~dp0build\Release"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 5 960x576 --prof bench_rm16_960.csv > "%~dp0_bench_rm16_960.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "%~dp0_bench_rm16_960.log"
copy /y bench_rm16_960.csv "%~dp0bench_rm16_960.csv" > nul
