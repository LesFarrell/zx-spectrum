@echo off
setlocal
pushd "%~dp0.."
if errorlevel 1 exit /b 1
set "VSWHERE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE_EXE%" for /f "usebackq delims=" %%I in (`"%VSWHERE_EXE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do if not defined VCVARS set "VCVARS=%%I"
if not defined VCVARS echo Visual C++ build tools not found.& exit /b 1
call "%VCVARS%"
if errorlevel 1 exit /b 1
if not exist tests\bin mkdir tests\bin
cl /nologo /std:c11 /W4 /I src /I third_party /I third_party\chips /Fo:tests\bin\ /Fe:tests\bin\core-tests.exe tests\test_core.c src\disk.c src\file_util.c src\microdrive.c src\szx_inflate.c src\tape.c src\wd1772.c src\zip.c user32.lib
if errorlevel 1 exit /b 1
tests\bin\core-tests.exe
set "RESULT=%ERRORLEVEL%"
del /q tests\bin\core-tests.exe tests\bin\core-tests.obj tests\bin\disk.obj tests\bin\file_util.obj tests\bin\microdrive.obj tests\bin\szx_inflate.obj tests\bin\tape.obj tests\bin\wd1772.obj tests\bin\zip.obj 2>nul
if not "%RESULT%"=="0" popd & exit /b %RESULT%
cl /nologo /std:c11 /W4 /I src /I third_party /I third_party\chips /Fo:tests\bin\ /Fe:tests\bin\timing-tests.exe tests\test_timing.c
if errorlevel 1 popd & exit /b 1
tests\bin\timing-tests.exe
set "RESULT=%ERRORLEVEL%"
del /q tests\bin\timing-tests.exe tests\bin\test_timing.obj 2>nul
cl /nologo /std:c11 /W4 /I src /I third_party /I third_party\chips /Fo:tests\bin\ /Fe:tests\bin\z80-timing-tests.exe tests\test_z80_timing.c
if errorlevel 1 popd & exit /b 1
tests\bin\z80-timing-tests.exe
set "RESULT=%ERRORLEVEL%"
del /q tests\bin\z80-timing-tests.exe tests\bin\test_z80_timing.obj 2>nul
popd
exit /b %RESULT%
