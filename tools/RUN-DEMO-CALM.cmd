@echo off
chcp 65001 >nul
REM Спокойная версия без дёрганья курсора.
REM Картинка обновляется только при реальных изменениях на экране
REM (двигайте окна, откройте видео - иначе экран "замёрзнет" - это
REM нормально, так работает Desktop Duplication на неподвижном десктопе).
cd /d C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m4-present-simple\build\Release
m4simple.exe --frames 1000000 --scale 0.55 --nowiggle
pause
