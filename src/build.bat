@echo off
call "%~dp0vcvars.bat" || exit /b 1
cd /d "%~dp0"
if "%UEOUT%"=="" set UEOUT=..\bin
if "%UEOBJ%"=="" set UEOBJ=obj
if not exist %UEOUT% mkdir %UEOUT%
if not exist %UEOBJ% mkdir %UEOBJ%
set MH=minhook\src\hook.c minhook\src\buffer.c minhook\src\trampoline.c minhook\src\hde\hde64.c
set CF=/nologo /O2 /MT /EHsc /Zi /std:c++17 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo%UEOBJ%\ /Fd%UEOBJ%\
if "%1"=="core" goto core
cl %CF% /LD loader.cpp %MH% /Fe%UEOUT%\UEScanner.dll /link /DEBUG /PDB:%UEOUT%\UEScanner.pdb || exit /b 1
:core
cl %CF% /LD core.cpp util.cpp ue.cpp hooks.cpp probe.cpp plugins.cpp core_seh.cpp %MH% /Fe%UEOUT%\UEScannerCore.dll /link /DEBUG /PDB:%UEOUT%\UEScannerCore_%RANDOM%.pdb psapi.lib advapi32.lib winmm.lib || exit /b 1
echo BUILD OK
