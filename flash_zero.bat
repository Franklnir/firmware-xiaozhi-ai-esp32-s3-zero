@echo off
echo ====================================================
echo Flashing XiaoZhi Firmware to Waveshare ESP32-S3-Zero
echo ====================================================

python flash.py
if errorlevel 1 (
    echo.
    echo Flashing gagal atau belum terdeteksi.
    pause
) else (
    echo.
    echo Flashing SELESAI SUKSES!
    pause
)
