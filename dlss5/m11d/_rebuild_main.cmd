@echo off
rem Force-rebuild m11d main.cpp (NMake dep scanner missed engine.h change).
del "C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11d\build-nmake\CMakeFiles\m11d.dir\main.cpp.obj" 2>nul
call "C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11d\_build_inc.cmd"
findstr /C:"main.cpp" /C:"EXIT" "C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11d\_build_inc.log"
