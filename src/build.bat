@echo off
setlocal

set "SRC_DIR=%~dp0"
for %%I in ("%SRC_DIR%..") do set "ROOT=%%~fI"

pushd "%ROOT%"
if errorlevel 1 exit /b 1

set "BUILD_DIR=src"
set "RES=%BUILD_DIR%\zxspecemu-res.o"
set "OUT=%BUILD_DIR%\zxspecemu.exe"

zig rc /:output-format coff /fo "%RES%" "src\resources.rc"
if errorlevel 1 (
    popd
    exit /b 1
)

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

set "BUILD_RESULT=%errorlevel%"
popd
exit /b %BUILD_RESULT%
