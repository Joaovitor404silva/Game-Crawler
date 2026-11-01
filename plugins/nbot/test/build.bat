@echo off
call "%~dp0..\..\..\src\vcvars.bat" || exit /b 1
set O=%TEMP%\nbot_cine_test
if not exist %O% mkdir %O%
cd /d %~dp0
cl /nologo /std:c++17 /EHsc /W3 /O1 /Fo%O%\ /Fe%O%\test_cine_math.exe test_cine_math.cpp || exit /b 1
%O%\test_cine_math.exe
