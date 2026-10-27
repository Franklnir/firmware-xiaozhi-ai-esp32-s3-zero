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
echo [1/3] Building project for ESP32-S3-Zero...
call idf.py build
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo [2/3] Merging binaries (4MB single factory binary)...
call idf.py merge-bin
if errorlevel 1 (
    echo MERGE FAILED
    exit /b 1
)
echo [3/3] Copying factory binary...
if not exist "releases\esp32-s3-zero" mkdir "releases\esp32-s3-zero"
copy /y "build\merged-binary.bin" "releases\esp32-s3-zero\firmware_esp32_s3_zero_factory.bin"
if errorlevel 1 (
    echo COPY FAILED
    exit /b 1
)
echo === SUCCESS: Factory binary releases\esp32-s3-zero\firmware_esp32_s3_zero_factory.bin created ===

