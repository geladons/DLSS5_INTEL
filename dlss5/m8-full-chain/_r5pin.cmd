@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\build\Release
echo === to0 split1 dispdbg ===
m8proto.exe --to 0 --split 1 --dispdbg > out\_r5_split1.txt 2>&1
echo exit=%errorlevel%
findstr /c:"disp " /c:"VK error" /c:"GPU-side" out\_r5_split1.txt
echo === to0 split1 nobias ===
m8proto.exe --to 0 --split 1 --dispdbg --nobias > out\_r5_split1_nb.txt 2>&1
echo exit=%errorlevel%
findstr /c:"disp " /c:"VK error" /c:"GPU-side" out\_r5_split1_nb.txt
