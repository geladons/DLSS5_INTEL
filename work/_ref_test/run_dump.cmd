@echo off
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\work\_ref_test
set PYTHONPATH=C:\Users\AI\Desktop\DLSS5_INTEL\work\mlx-dlss\python
start "" /min cmd /c ".venv\Scripts\python.exe dump_torch.py > dump_torch.log 2>&1"
exit /b 0
