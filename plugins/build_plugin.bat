@echo off
setlocal
set N=%~1
set P=%~dp0.
set O=%~dp0..\bin\plugins
if not exist %O% mkdir %O%
if not exist %O%\obj_%N% mkdir %O%\obj_%N%
call "%~dp0..\src\vcvars.bat" || exit /b 1
cd /d %P%\%N% || exit /b 2
cl /nologo /O2 /MT /EHsc /std:c++17 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I%P% /LD *.cpp /Fo%O%\obj_%N%\ /Fe%O%\%N%.dll /link /PDB:%O%\obj_%N%\%N%_%RANDOM%.pdb /DEBUG
