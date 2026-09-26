@echo off
set "REPO=%~dp0\..\..\.."
set VK_LOADER_DEBUG=all
%REPO%\dlss5\m11-layer\d3d9_test\vk32probe.exe
echo PROBE_EXIT=%errorlevel%
