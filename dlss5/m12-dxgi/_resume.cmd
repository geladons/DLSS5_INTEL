@echo off
rem M12: resume AI processing (back to slideshow mode).
if exist "%TEMP%\m12_pause.flag" del "%TEMP%\m12_pause.flag"
echo M12 processing RESUMED (flag cleared)
