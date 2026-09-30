@echo off
:: SPDX-License-Identifier: Apache-2.0
setlocal
rem Standalone runner for DLSS 5 scale-reconstruction test
cd /d "%~dp0"
python test_scale_reconstruction.py %1
exit /b %ERRORLEVEL%
