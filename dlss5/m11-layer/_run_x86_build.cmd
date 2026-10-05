@echo off
cd /d "%~dp0"
call _build_x86.cmd > _bx86_run.log 2>&1
echo done >> _bx86_run.log
