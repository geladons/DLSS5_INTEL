@echo off
setlocal
rem M9-UNET: configure + build only. cmd batch - immune to the exec wrapper's
rem PowerShell $-mangling. VS generator first, NMake fallback (broken COM
rem instance discovery on this host - see m8b-live/build.cmd note).

set "REPO=%~dp0\..\.."
set "SRC=%REPO%\dlss5\m9-unet"
set "BUILD=%SRC%\build"
set "BUILDNM=%SRC%\build-nmake"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo === M9-UNET build ===
echo started %DATE% %TIME%

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64
if not errorlevel 1 (
    "%CMAKE%" --build "%BUILD%" --config Release
    if errorlevel 1 ( set FINAL=1 & goto :done )
    if not exist "%BUILD%\Release\m9unet.exe" ( set FINAL=1 & goto :done )
    set FINAL=0
    goto :done
)

echo === VS generator unavailable - NMake fallback ===
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 ( set FINAL=1 & goto :done )
"%CMAKE%" -S "%SRC%" -B "%BUILDNM%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 ( set FINAL=1 & goto :done )
"%CMAKE%" --build "%BUILDNM%"
if errorlevel 1 ( set FINAL=1 & goto :done )
if not exist "%BUILDNM%\m9unet.exe" ( set FINAL=1 & goto :done )
if not exist "%BUILD%\Release" mkdir "%BUILD%\Release"
for %%F in ("%BUILDNM%\*.spv") do copy /y "%%~fF" "%BUILD%\Release\" >nul
copy /y "%BUILDNM%\m9unet.exe" "%BUILD%\Release\" >nul
set FINAL=0

:done
echo === final exit code: %FINAL% ===
exit /b %FINAL%
