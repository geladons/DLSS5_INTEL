@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /EHsc /Fe:probe.exe probe.cpp > build_probe.log 2>&1
echo exit=%errorlevel%
