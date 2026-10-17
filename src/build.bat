@echo off
setlocal

pushd "%~dp0"
if errorlevel 1 exit /b 1

if not exist "..\bin" mkdir "..\bin"
if errorlevel 1 (
    popd
    exit /b 1
)

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
    file_util.c ^
    spectrum.c ^
    szx_inflate.c ^
    zip.c ^
    disk.c ^
    microdrive.c ^
    tape.c ^
    zxspectrum-res.o ^
    -lgdi32 ^
    -lcomdlg32 ^
    -lcomctl32 ^
    -lshell32 ^
    -luser32 ^
    -lwinmm ^
    -Wl,/subsystem:windows ^
    -o "..\bin\zx-spectrum.exe"

set "BUILD_RESULT=%errorlevel%"

if exist "zxspectrum-res.o" del /q "zxspectrum-res.o"
if exist "zx-spectrum.pdb" del /q "zx-spectrum.pdb"
if exist "..\bin\zx-spectrum.pdb" del /q "..\bin\zx-spectrum.pdb"

if "%BUILD_RESULT%"=="0" (
    copy /y "..\third_party\scintilla\bin\Scintilla.dll" "..\bin\Scintilla.dll" >nul
    xcopy /y /q /i /e "roms" "..\bin\roms" >nul
)

popd
exit /b %BUILD_RESULT%
