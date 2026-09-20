@echo off
"C:\Program Files\Git\cmd\git.exe" -C C:\Users\AI\Desktop\DLSS5_INTEL log --oneline -3
echo === status main.cpp ===
"C:\Program Files\Git\cmd\git.exe" -C C:\Users\AI\Desktop\DLSS5_INTEL status --short dlss5/m8-full-chain/main.cpp
echo === diff stat HEAD ===
"C:\Program Files\Git\cmd\git.exe" -C C:\Users\AI\Desktop\DLSS5_INTEL diff --stat HEAD -- dlss5/m8-full-chain/main.cpp
