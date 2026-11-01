@echo off
call "%~dp0..\src\vcvars.bat" || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
set S=..\src
set MH=%S%\minhook\src\hook.c %S%\minhook\src\buffer.c %S%\minhook\src\trampoline.c %S%\minhook\src\hde\hde64.c
cl /nologo /Od /Ob0 /MT /EHsc /Zi /std:c++17 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I%S% /I%S%\core /I%S%\engines /I%S%\engines\ue /Foobj\ /Fdobj\ test_all.cpp test_stubs.cpp %S%\core\core.cpp %S%\core\util.cpp %S%\core\hooks.cpp %S%\core\probe.cpp %S%\core\core_seh.cpp %S%\core\bot.cpp %MH% /Fe:test_all.exe /link /INCREMENTAL:NO /DEBUG psapi.lib advapi32.lib winmm.lib user32.lib || exit /b 1
echo TEST BUILD OK
