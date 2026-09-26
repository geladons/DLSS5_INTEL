@echo off
set "REPO=%~dp0\..\.."
rem DLSS 5 Manager (M13) - stdlib-tkinter app on the user Python 3.12.
rem Nothing installs, nothing elevates; HKCU registration only.
start "DLSS5 Manager" /min "%USERPROFILE%\AppData\Local\Programs\Python\Python312\pythonw.exe" "%~dp0m13.pyw"
