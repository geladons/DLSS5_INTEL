@echo off
rem Run the 32-bit D3D9 test through DXVK with the m11 layer in live mode.
setlocal
set "REPO=%~dp0\..\..\.."
set NR_LAYER_LIVE=1
%REPO%\dlss5\m11-layer\d3d9_test\d3d9_test.exe
