@echo off
call "%~dp0..\..\src\vcvars.bat" || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /I..\..\src\agent /Foobj\ /Fdobj\ test_agent.cpp /Fe:test_agent.exe || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /I..\..\src\agent /Foobj\ /Fdobj\ simhost.cpp /Fe:simhost.exe || exit /b 1
echo AGENT TEST BUILD OK
