@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0_lswin.ps1" > "%~dp0out\_lswin.txt" 2>&1
type "%~dp0out\_lswin.txt"
