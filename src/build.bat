@echo off
setlocal

pushd "%~dp0"
if errorlevel 1 exit /b 1

zig rc /:output-format coff /fo zxspectrum-res.o resources.rc
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
    -I..\third_party\scintilla\include ^
    main.c ^
    spectrum.c ^
    szx_inflate.c ^
    disk.c ^
    tape.c ^
    zxspectrum-res.o ^
    -lgdi32 ^
    -lcomdlg32 ^
    -lcomctl32 ^
    -lshell32 ^
    -luser32 ^
    -lwinmm ^
    -Wl,/subsystem:windows ^
    -o zx-spectrum.exe

if not errorlevel 1 copy /y "..\third_party\scintilla\bin\Scintilla.dll" "Scintilla.dll" >nul

set "BUILD_RESULT=%errorlevel%"
if exist "zxspectrum-res.o" del /q "zxspectrum-res.o"
if exist "zx-spectrum.pdb" del /q "zx-spectrum.pdb"
popd
exit /b %BUILD_RESULT%
