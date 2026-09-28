@echo off
setlocal
rem Standalone runner for DLSS 5 Frame Generation test
cd /d "%~dp0"
python test_framegen.py
exit /b %ERRORLEVEL%
