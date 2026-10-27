@echo off
if exist C:\Espressif\v5.5.1\esp-idf\export.bat (
    call C:\Espressif\v5.5.1\esp-idf\export.bat
) else if exist C:\esp\.espressif\v5.5.2\esp-idf\export.bat (
    call C:\esp\.espressif\v5.5.2\esp-idf\export.bat
)
set PYTHONIOENCODING=utf-8
python scripts\build.py waveshare/esp32-s3-zero --name esp32-s3-zero %*
