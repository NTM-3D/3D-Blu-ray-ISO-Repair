@echo off
setlocal

for /f "tokens=3" %%i in ('findstr /B /C:"#define BR3D_VER_MAJOR" src\version.h') do set "MAJOR=%%i"
for /f "tokens=3" %%i in ('findstr /B /C:"#define BR3D_VER_MINOR" src\version.h') do set "MINOR=%%i"
set "VER=%MAJOR%.%MINOR%"

if not defined CC set "CC=gcc"
if not defined RC set "RC=windres"
where %CC% >nul 2>nul
if errorlevel 1 (
    echo Error: %CC% was not found on PATH. Install a C11 toolchain such as w64devkit or MinGW-w64,
    echo or set the CC and RC environment variables to your compiler and windres.
    exit /b 1
)
where %RC% >nul 2>nul
if errorlevel 1 (
    echo Error: %RC% was not found on PATH.
    exit /b 1
)

echo Building 3D Blu-ray ISO Repair v%VER% with "%CC%" ...
"%RC%" src\version.rc -O coff -o src\version_rc.o
if errorlevel 1 (
    echo Resource build failed.
    exit /b 1
)
"%CC%" -O2 -std=c11 -Wall -Wextra -o 3d_bluray_iso_repair_v%VER%.exe ^
    src\udf.c src\udf_repair.c src\clpi.c src\repair.c src\report.c src\main.c src\version_rc.o
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo Built: 3d_bluray_iso_repair_v%VER%.exe
endlocal