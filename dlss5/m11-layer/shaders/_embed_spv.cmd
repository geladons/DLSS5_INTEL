@echo off
rem Recompile the gpuwarp shaders and regenerate the embedded-spv header.
rem Called by _build_x86.cmd / build.cmd before the C compiles.
setlocal
set "HERE=%~dp0"
set "PY=python"
where %PY% >nul 2>&1 || set "PY=py -3"
%PY% "%HERE%_embed_spv.py"
if errorlevel 1 (
    echo [FAIL] gpuwarp spv embed
    exit /b 1
)
exit /b 0
