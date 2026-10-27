@echo off
set MSYSTEM=
set MSYSCON=
set MINGW_PREFIX=
set MINGW_CHOST=
set MINGW_PACKAGE_PREFIX=
set MSYSTEM_CARCH=
set MSYSTEM_CHOST=
cd /d "%~dp0"
if exist C:\Espressif\v5.5.1\esp-idf\export.bat (
    call C:\Espressif\v5.5.1\esp-idf\export.bat >nul 2>&1
) else if exist C:\esp\.espressif\v5.5.2\esp-idf\export.bat (
    call C:\esp\.espressif\v5.5.2\esp-idf\export.bat >nul 2>&1
)
set PORT=%1
if "%PORT%"=="" set PORT=COM13
echo Using port %PORT% for ESP32-S3-Zero (4MB Flash)...

if exist build\merged-binary.bin (
    python -m esptool --chip esp32s3 --port %PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 build\merged-binary.bin
    if errorlevel 1 (
        echo Retrying at 115200 baud...
        python -m esptool --chip esp32s3 --port %PORT% -b 115200 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 build\merged-binary.bin
    )
) else (
    python -m esptool --chip esp32s3 --port %PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0xd000 build\ota_data_initial.bin 0x10000 build\xiaozhi.bin 0x390000 build\generated_assets.bin
    if errorlevel 1 (
        echo Retrying at 115200 baud...
        python -m esptool --chip esp32s3 --port %PORT% -b 115200 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0xd000 build\ota_data_initial.bin 0x10000 build\xiaozhi.bin 0x390000 build\generated_assets.bin
    )
)
echo === FLASH COMPLETE ===

