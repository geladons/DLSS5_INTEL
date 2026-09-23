@echo off
setlocal
rem M11-LAYER: configure + build the Windows Vulkan present layer.
rem Same VS-discovery fallback as m8b-live (COM discovery is broken here).

set "REPO=C:\Users\AI\Desktop\DLSS5_INTEL"
set "SRC=%REPO%\dlss5\m11-layer"
set "BUILD=%SRC%\build"
set "BUILDNM=%SRC%\build-nmake"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo === M11-LAYER build ===
echo started %DATE% %TIME%

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64
if not errorlevel 1 (
    "%CMAKE%" --build "%BUILD%" --config Release
    if errorlevel 1 ( set FINAL=1 & goto :done )
    if not exist "%BUILD%\Release\nr_layer_win.dll" ( set FINAL=1 & goto :done )
    rem manifest sits next to the dll; library_path is relative to the json
    copy /y "%SRC%\VkLayer_dlssnr_win.json" "%BUILD%\Release\" >nul
    set FINAL=0
    goto :done
)

echo === VS generator unavailable (instance discovery broken) - NMake fallback ===
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 ( set FINAL=1 & goto :done )
"%CMAKE%" -S "%SRC%" -B "%BUILDNM%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 ( set FINAL=1 & goto :done )
"%CMAKE%" --build "%BUILDNM%"
if errorlevel 1 ( set FINAL=1 & goto :done )
if not exist "%BUILDNM%\nr_layer_win.dll" ( set FINAL=1 & goto :done )
if not exist "%BUILD%\Release" mkdir "%BUILD%\Release"
copy /y "%BUILDNM%\nr_layer_win.dll" "%BUILD%\Release\" >nul
copy /y "%SRC%\VkLayer_dlssnr_win.json" "%BUILD%\Release\" >nul
set FINAL=0

:done
echo === build exit code: %FINAL% (ended %DATE% %TIME%) ===
exit /b %FINAL%
