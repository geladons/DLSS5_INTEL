@echo off
setlocal
rem Standalone runner for DLSS 5 framegen residual-transfer test
cd /d "%~dp0"
python test_framegen_transfer.py %1
exit /b %ERRORLEVEL%
