@echo off
:: SPDX-License-Identifier: Apache-2.0
setlocal
rem Standalone runner for the DLSS 5 graceful-init-failure regression test
rem (d5c_chain.dll must never exit() the host process on bad weights/spv).
cd /d "%~dp0"
python test_graceful_fail.py %1
exit /b %ERRORLEVEL%
