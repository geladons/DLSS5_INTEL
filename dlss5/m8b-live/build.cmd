@echo off
setlocal
rem M8B-LIVE: configure + build only. Run orchestration is separate
rem (runm8b.cmd) so multiple runs append to docs\m8b-live.log.
rem cmd batch - immune to the exec wrapper's PowerShell $-mangling.
rem
rem 2026-09-21: VS-instance discovery (Setup.Configuration COM) is BROKEN on
rem this host (clsid registration missing; vswhere/cmake enumerate 0
rem instances even after a full reinstall + fresh instance registration).
rem FALLBACK: NMake Makefiles generator + vcvars64 (filesystem-based, no COM).
rem The canonical VS generator is still tried first; the fallback keeps
rem builds working until the COM registration is repaired.

set "REPO=%~dp0\..\.."
set "SRC=%REPO%\dlss5\m8b-live"
set "BUILD=%SRC%\build"
set "BUILDNM=%SRC%\build-nmake"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo === M8B-LIVE build ===
echo started %DATE% %TIME%
"%CMAKE%" --version
"%VULKAN_SDK%\Bin\glslangValidator.exe" --version | findstr /i version

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64
if not errorlevel 1 (
    "%CMAKE%" --build "%BUILD%" --config Release
    if errorlevel 1 ( set FINAL=1 & goto :done )
    if not exist "%BUILD%\Release\m8blive.exe" ( set FINAL=1 & goto :done )
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
if not exist "%BUILDNM%\m8blive.exe" ( set FINAL=1 & goto :done )
rem keep runm8b.cmd / RUN-DEMO.cmd working unchanged: sync exe + shaders
if not exist "%BUILD%\Release" mkdir "%BUILD%\Release"
for %%F in ("%BUILDNM%\*.spv") do copy /y "%%~fF" "%BUILD%\Release\" >nul
copy /y "%BUILDNM%\m8blive.exe" "%BUILD%\Release\" >nul
set FINAL=0

:done
echo === build exit code: %FINAL% (ended %DATE% %TIME%) ===
exit /b %FINAL%
