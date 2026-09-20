@echo off
setlocal enabledelayedexpansion
rem M0 coopmat probe build+run orchestrator (cmd batch: immune to the exec
rem wrapper's PowerShell $-mangling). Loops up to 3 attempts; the probe writes
rem chosen.txt beside probe.exe, which we feed back into cmake -D defines.
rem NOTE: inside the FOR /L block all config vars MUST use !VAR! (delayed
rem expansion); %VAR% would be expanded once at block-parse time.

set "REPO=C:\Users\AI\Desktop\DLSS5_INTEL"
set "SRC=%REPO%\dlss5\m0-coopmat-probe"
set "BUILD=%SRC%\build"
set "LOG=%REPO%\docs\m0-coopmat-probe.log"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

rem ---- defaults (typical Arc f16 XMX shape) ----
set CM=16
set CN=16
set CK=16
set CSG=32
set CCF32=1

if not exist "%BUILD%\Release\chosen.txt" goto :start
for /f "usebackq tokens=1,2 delims==" %%a in ("%BUILD%\Release\chosen.txt") do (
    if /i "%%a"=="M" set CM=%%b
    if /i "%%a"=="N" set CN=%%b
    if /i "%%a"=="K" set CK=%%b
    if /i "%%a"=="SG" set CSG=%%b
    if /i "%%a"=="CF32" set CCF32=%%b
)

:start
echo === M0 coopmat probe build+run log ===> "%LOG%"
echo started %DATE% %TIME% >> "%LOG%"
"%CMAKE%" --version >> "%LOG%" 2>&1
"%VULKAN_SDK%\Bin\glslangValidator.exe" --version >> "%LOG%" 2>&1

set FINAL=99
for /l %%i in (1,1,3) do (
    echo. >> "%LOG%"
    echo --- attempt %%i: config M=!CM! N=!CN! K=!CK! SUBGROUP=!CSG! CF32=!CCF32! --- >> "%LOG%"

    echo ^>^> cmake configure >> "%LOG%"
    "%CMAKE%" -S "%SRC%" -B "%BUILD%" -G "Visual Studio 16 2019" -A x64 -DCFG_M=!CM! -DCFG_N=!CN! -DCFG_K=!CK! -DCFG_SUBGROUP=!CSG! -DCFG_CF32=!CCF32! >> "%LOG%" 2>&1
    if errorlevel 1 ( set FINAL=1 & goto :done )

    echo ^>^> cmake build >> "%LOG%"
    "%CMAKE%" --build "%BUILD%" --config Release >> "%LOG%" 2>&1
    if errorlevel 1 ( set FINAL=1 & goto :done )

    if not exist "%BUILD%\Release\probe.exe" ( set FINAL=1 & goto :done )

    echo ^>^> run probe >> "%LOG%"
    pushd "%BUILD%\Release"
    ".\probe.exe" >> "%LOG%" 2>&1
    set FINAL=!errorlevel!
    popd
    echo probe exit code: !FINAL! >> "%LOG%"

    if !FINAL! equ 0 goto :done
    if !FINAL! neq 2 goto :done

    rem config mismatch: reload chosen.txt and retry
    for /f "usebackq tokens=1,2 delims==" %%a in ("%BUILD%\Release\chosen.txt") do (
        if /i "%%a"=="M" set CM=%%b
        if /i "%%a"=="N" set CN=%%b
        if /i "%%a"=="K" set CK=%%b
        if /i "%%a"=="SG" set CSG=%%b
        if /i "%%a"=="CF32" set CCF32=%%b
    )
)

:done
echo. >> "%LOG%"
echo === final exit code: %FINAL% (ended %DATE% %TIME%) === >> "%LOG%"
type "%LOG%"
exit /b %FINAL%
