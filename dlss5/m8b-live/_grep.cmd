@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8b-live
C:\Progra~1\Git\cmd\git.exe grep -n -E "VK_FORMAT_B8G8R8A8|VK_FORMAT_R8G8B8A8|surfaceFormat|swapChain|fbcancel|lastPresented|BGRA|RGBA" -- main.cpp
