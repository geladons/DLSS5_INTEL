@echo off
setlocal
set "REPO=%~dp0\..\.."
cd /d %REPO%\dlss5\m8-full-chain
call build_m8.cmd --no-run
cd build\Release
m8proto.exe --to 4 --dispdbg > out\_r5_dbgarena.txt 2>&1
echo exit=%errorlevel%
findstr /c:"debug arena" /c:"GPU-side" out\_r5_dbgarena.txt
%USERPROFILE%\AppData\Local\Programs\Python\Python312\python.exe -c "import numpy as np; [print(n, np.fromfile('out/%s.bin'%n, dtype=np.float32)[:6], 'absmax', np.abs(np.fromfile('out/%s.bin'%n, dtype=np.float32)).max()) for n in ['dbg_x','dbg_ada','dbg_g2','dbg_b4ds']]; a=np.fromfile('out/dbg_x16.bin',dtype=np.uint16); print('dbg_x16', a[:8], 'nonzero', (a!=0).mean())"
