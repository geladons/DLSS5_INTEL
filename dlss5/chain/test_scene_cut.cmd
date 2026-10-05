@echo off
:: SPDX-License-Identifier: Apache-2.0
setlocal
rem Standalone runner for DLSS 5 scene-cut temporal-history reset test
cd /d "%~dp0"
python test_scene_cut.py %1
exit /b %ERRORLEVEL%
