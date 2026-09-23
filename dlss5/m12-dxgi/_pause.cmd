@echo off
rem M12: pause AI processing - frames pass through untouched (full fps).
type nul > "%TEMP%\m12_pause.flag"
echo M12 processing PAUSED (flag %TEMP%\m12_pause.flag set)
