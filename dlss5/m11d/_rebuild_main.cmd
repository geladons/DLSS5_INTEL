@echo off
set "REPO=%~dp0\..\.."
rem Force-rebuild m11d main.cpp (NMake dep scanner missed engine.h change).
del "%REPO%\dlss5\m11d\build-nmake\CMakeFiles\m11d.dir\main.cpp.obj" 2>nul
call "%REPO%\dlss5\m11d\_build_inc.cmd"
findstr /C:"main.cpp" /C:"EXIT" "%REPO%\dlss5\m11d\_build_inc.log"
