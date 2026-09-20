@echo off
REM =====================================================================
REM  Restore device data after flashing (API key, optional face image).
REM
REM  Why this exists
REM  ===============
REM  Flashing erases the whole NAND, including /data. By design the API
REM  key is NOT in the firmware (anything in the firmware is public),
REM  so it has to be pushed again after every flash.
REM
REM  Usage (from the repo root):
REM      scripts\restore_device.bat
REM      scripts\restore_device.bat myface.png
REM
REM  NOTE: this file is deliberately ASCII-only. Windows PowerShell /
REM  cmd read .ps1/.bat using the system ANSI codepage unless a BOM is
REM  present, so a UTF-8 file with non-ASCII text gets mis-decoded and
REM  can fail to parse. Keep it ASCII.
REM =====================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0.."

set "KEYFILE=tmp\stepfun.key"
set "FACEPNG=%~1"

echo.
echo === 1. Check device ===
set "STATE="
for /f "usebackq delims=" %%s in (`adb get-state 2^>nul`) do set "STATE=%%s"
if not "!STATE!"=="device" (
    echo   [X] No device found. Check the USB cable and that adb sees it.
    adb devices
    exit /b 1
)
echo   device online

echo.
echo === 2. Create directory ===
adb shell "mkdir -p /data/etc/superchild"
if errorlevel 1 (
    echo   [X] mkdir failed. Is /data mounted?
    exit /b 1
)

echo.
echo === 3. Push API key ===
if not exist "%KEYFILE%" (
    echo   [!] %KEYFILE% not found - skipping.
    echo       Put the key on a single line in that file.
    goto :face
)

set "KEY="
for /f "usebackq delims=" %%k in ("%KEYFILE%") do (
    if not defined KEY set "KEY=%%k"
)
if "!KEY!"=="" (
    echo   [X] %KEYFILE% is empty.
    exit /b 1
)

adb shell "echo !KEY! > /data/etc/superchild/stepfun.key"

REM Read back and compare - "pushed without error" is not the same as "written".
set "BACK="
for /f "usebackq delims=" %%b in (`adb shell "cat /data/etc/superchild/stepfun.key"`) do (
    if not defined BACK set "BACK=%%b"
)

if "!BACK!"=="!KEY!" (
    echo   [OK] key written and read-back verified
) else (
    echo   [X] read-back mismatch!
    echo       wrote : !KEY!
    echo       read  : !BACK!
    exit /b 1
)

:face
if "%FACEPNG%"=="" goto :done
if not exist "%FACEPNG%" (
    echo   [X] image not found: %FACEPNG%
    exit /b 1
)

echo.
echo === 4. Convert and push image ===
set "BIN=%TEMP%\face.bin"
python "%~dp0png_to_facebin.py" "%FACEPNG%" "%BIN%"
if errorlevel 1 (
    echo   [X] conversion failed. Needs Pillow:  pip install pillow
    exit /b 1
)

adb shell "rm -f /data/etc/superchild/face.bin"
adb push "%BIN%" /data/etc/superchild/face.bin
adb shell "ls -l /data/etc/superchild/face.bin"

:done
echo.
echo === Done ===
echo   The image (if pushed) appears on the next boot.
echo   To apply now:   adb shell reboot
echo.
endlocal
