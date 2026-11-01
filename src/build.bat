@echo off
setlocal

pushd "%~dp0.."
if errorlevel 1 exit /b 1

where premake5 >nul 2>nul
if errorlevel 1 goto :missing_premake

premake5 vs2022
if errorlevel 1 goto :failed

set "MSBUILD_EXE="
set "VSWHERE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE_EXE%" for /f "usebackq delims=" %%I in (`"%VSWHERE_EXE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do if not defined MSBUILD_EXE set "MSBUILD_EXE=%%I"
if not defined MSBUILD_EXE for /f "delims=" %%I in ('where msbuild 2^>nul') do if not defined MSBUILD_EXE set "MSBUILD_EXE=%%I"
if not defined MSBUILD_EXE goto :missing_msbuild

"%MSBUILD_EXE%" "build\msvc\zx-spectrum.sln" /nologo /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
if errorlevel 1 goto :failed
if exist "redist\zx-spectrum.pdb" del /q "redist\zx-spectrum.pdb"

popd
exit /b 0

:missing_premake
echo premake5.exe was not found on PATH.
goto :failed

:missing_msbuild
echo MSBuild was not found. Install Visual Studio with Desktop development with C++.
goto :failed

:failed
popd
exit /b 1
