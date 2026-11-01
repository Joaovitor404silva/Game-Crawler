@echo off
if defined VCToolsInstallDir exit /b 0
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VSW%\vswhere.exe" (echo Visual Studio not found & exit /b 1)
set "PATH=%VSW%;%PATH%"
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo MSVC x64 tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
