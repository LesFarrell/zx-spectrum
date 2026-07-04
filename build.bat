@echo off
setlocal

set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%build"
set "OUT=%BUILD_DIR%\zxspecemu.exe"
set "RES=%BUILD_DIR%\zxspecemu-res.o"

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
    if errorlevel 1 exit /b 1
)

zig rc /:output-format coff /fo "%RES%" "%ROOT%resources.rc"
if errorlevel 1 exit /b 1

zig cc ^
    -std=c11 ^
    -O2 ^
    -Wall ^
    -Wextra ^
    -Isrc ^
    -Ithird_party/chips ^
    src\main.c ^
    src\spectrum.c ^
    "%RES%" ^
    -lgdi32 ^
    -lcomdlg32 ^
    -lcomctl32 ^
    -luser32 ^
    -lwinmm ^
    -Wl,/subsystem:windows ^
    -o "%OUT%"

exit /b %errorlevel%
pause

