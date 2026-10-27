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
python apply_board_config.py
call idf.py build
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
call idf.py merge-bin
set PORT=%1
if "%PORT%"=="" set PORT=COM13
echo Using port %PORT%...

python flash.py %PORT%
if errorlevel 1 (
    echo FLASH FAILED
    exit /b 1
)
echo === DONE ===

