@echo off
setlocal
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8-full-chain\shaders
findstr /n "uint(Half" gemm.comp
findstr /n "uint(Half" gather_residual.comp
findstr /n "uint(Half" cosine.comp
findstr /n "uint(Half" cosine_win.comp
findstr /n "uint(Half" partition.comp
findstr /n "uint(Half" transpose_we.comp
findstr /n "uint(Half" elementwise.comp
findstr /n "uint(Half" merge.comp
findstr /n "float16BitsToUint" softmax.comp elementwise.comp merge.comp gemm.comp
