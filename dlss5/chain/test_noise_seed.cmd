@echo off
setlocal
rem Standalone runner for DLSS 5 pinned noise-seed determinism test
cd /d "%~dp0"
python test_noise_seed.py %1
exit /b %ERRORLEVEL%
