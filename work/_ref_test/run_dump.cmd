@echo off
set "REPO=%~dp0\..\.."
cd /d %REPO%\work\_ref_test
set PYTHONPATH=%REPO%\work\mlx-dlss\python
start "" /min cmd /c ".venv\Scripts\python.exe dump_torch.py > dump_torch.log 2>&1"
exit /b 0
