@echo off
setlocal
rem ddarec: configure + build the DDA stream recorder.
set "SRC=%~dp0"
set "BUILD=%SRC%build"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64
if errorlevel 1 ( call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && "%CMAKE%" -S "%SRC%" -B "%BUILD%-nmake" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release && "%CMAKE%" --build "%BUILD%-nmake" && copy /y "%BUILD%-nmake%\ddarec.exe" "%BUILD%\Release\" & exit /b %errorlevel% )
"%CMAKE%" --build "%BUILD%" --config Release
exit /b %errorlevel%
