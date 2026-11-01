@echo off
call "%~dp0vcvars.bat" || exit /b 1
cd /d "%~dp0"
if "%S2OUT%"=="" set S2OUT=..\bin
if "%S2OBJ%"=="" set S2OBJ=obj
if not exist %S2OUT% mkdir %S2OUT%
if not exist %S2OBJ% mkdir %S2OBJ%
set MH=minhook\src\hook.c minhook\src\buffer.c minhook\src\trampoline.c minhook\src\hde\hde64.c
set CF=/nologo /O2 /MT /EHsc /Zi /std:c++17 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo%S2OBJ%\ /Fd%S2OBJ%\
if "%1"=="core" goto core
cl %CF% /LD loader.cpp %MH% /Fe%S2OUT%\S2Dev.dll /link /DEBUG /PDB:%S2OUT%\S2Dev.pdb || exit /b 1
:core
cl %CF% /LD core.cpp util.cpp ue.cpp hooks.cpp probe.cpp plugins.cpp core_seh.cpp %MH% /Fe%S2OUT%\S2DevCore.dll /link /DEBUG /PDB:%S2OUT%\S2DevCore_%RANDOM%.pdb psapi.lib advapi32.lib winmm.lib || exit /b 1
echo BUILD OK
