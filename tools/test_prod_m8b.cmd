@echo off
setlocal
cd /d "C:\Users\<owner>\Desktop\production\runtime\m8b"
m8blive.exe --novideo --frames 5 --wiggle-idle 1 > "C:\Users\<owner>\Desktop\production\logs\prod_m8b.log" 2>&1
echo EXITCODE=%ERRORLEVEL% >> "C:\Users\<owner>\Desktop\production\logs\prod_m8b.log"
