@echo off
setlocal

pushd "%~dp0"
if errorlevel 1 exit /b 1

zig rc /:output-format coff /fo zxspecemu-res.o resources.rc
if errorlevel 1 (
    popd
    exit /b 1
)

zig cc ^
    -std=c11 ^
    -O2 ^
    -Wall ^
    -Wextra ^
    -I. ^
    -I..\third_party\chips ^
    main.c ^
    spectrum.c ^
    tape.c ^
    zxspecemu-res.o ^
    -lgdi32 ^
    -lcomdlg32 ^
    -lcomctl32 ^
    -luser32 ^
    -lwinmm ^
    -Wl,/subsystem:windows ^
    -o zxspecemu.exe

set "BUILD_RESULT=%errorlevel%"
popd
exit /b %BUILD_RESULT%
