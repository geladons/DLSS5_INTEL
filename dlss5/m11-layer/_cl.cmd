@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >/dev/null
cl /nologo /std:c11 /c /I"C:\VulkanSDK\1.4.357.0\Include" "C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11-layer\nr_layer_win.c" /Fo"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m11-layer\_t.obj" 2>&1
