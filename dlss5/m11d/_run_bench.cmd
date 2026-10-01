@echo off
cd /d "%~dp0build\Release"
m11d.exe --weights "c:\Users\<owner>\Desktop\DLSS5_INTEL\work\mlxw\dlssnr-logical.safetensors" --bench 10 960x576 --prof "out\bench_960x576.csv" > "out\_bench_960.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "out\_bench_960.log"
