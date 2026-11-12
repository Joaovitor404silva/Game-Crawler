@echo off
call "%~dp0vcvars.bat" || exit /b 1
cd /d "%~dp0"
if "%UEOUT%"=="" set UEOUT=..\bin
if "%UEOBJ%"=="" set UEOBJ=obj
if not exist %UEOUT% mkdir %UEOUT%
if not exist %UEOBJ% mkdir %UEOBJ%
set MH=minhook\src\hook.c minhook\src\buffer.c minhook\src\trampoline.c minhook\src\hde\hde64.c
set CF=/nologo /Icore /Iengines /I. /O2 /MT /EHsc /Zi /std:c++17 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo%UEOBJ%\ /Fd%UEOBJ%\
if "%1"=="core" goto core
:agentd
cl /nologo /Iagent /O2 /MT /EHsc /std:c++17 /W3 /Fo%UEOBJ%\ /Fd%UEOBJ%\ agent\agentd.cpp agent\wgc.cpp /Fe%UEOUT%\agentd.exe || exit /b 1
if "%1"=="agentd" (echo BUILD OK & exit /b 0)
cl %CF% /LD core\loader.cpp %MH% /Fe%UEOUT%\GameCrawler.dll /link /DEBUG /PDB:%UEOUT%\GameCrawler.pdb || exit /b 1
if not exist %UEOUT%\proxy mkdir %UEOUT%\proxy
cl %CF% /LD core\proxy.cpp /Fe%UEOUT%\proxy\version.dll /link /NOIMPLIB /NOEXP || exit /b 1
:core
cl %CF% /LD core\core.cpp core\util.cpp core\hooks.cpp core\probe.cpp core\plugins.cpp core\core_seh.cpp core\bot.cpp engines\engine.cpp engines\ue\ue.cpp %MH% /Fe%UEOUT%\GameCrawlerCore.dll /link /DEBUG /PDB:%UEOUT%\GameCrawlerCore_%RANDOM%.pdb psapi.lib advapi32.lib winmm.lib user32.lib || exit /b 1
echo BUILD OK
