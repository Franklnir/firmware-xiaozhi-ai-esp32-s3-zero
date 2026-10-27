@echo off
cd /d "%~dp0"
if exist C:\Espressif\v5.5.1\esp-idf\export.bat (
    call C:\Espressif\v5.5.1\esp-idf\export.bat >nul 2>&1
) else if exist C:\esp\.espressif\v5.5.2\esp-idf\export.bat (
    call C:\esp\.espressif\v5.5.2\esp-idf\export.bat >nul 2>&1
)

echo ====================================================================
echo Flashing XiaoZhi ESP32-S3-Zero (Baud Rate: 115200 / Auto)
echo ====================================================================

python flash.py %1

pause

