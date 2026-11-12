@echo off
setlocal
set T=%~dp0
call "%T%build_tests.bat" || exit /b 1
set SC=%TEMP%\crawler_scenario.jsonl
"%T%test_all.exe" --scenario "%SC%" || exit /b 1
call "%T%agent\build_agent_tests.bat" || exit /b 1
"%T%agent\test_agent.exe" || exit /b 1
rem own agentd build: bin\agentd.exe may be running against a live game
set UEOUT=%T%agent\bin
set UEOBJ=%T%agent\obj
call "%T%..\src\build.bat" agentd || exit /b 1
set AGENTD=%T%agent\bin\agentd.exe
node "%T%agent\test_agentd.js" || exit /b 1
node "%T%test_scenario.js" "%SC%" || exit /b 1
node "%T%test_mcp.js" || exit /b 1
rem vision signals: only with the optional tools\.venv-vision (see tools\agent_vision.py)
set VPY=%T%..\tools\.venv-vision\Scripts\python.exe
if exist "%VPY%" ("%VPY%" -I "%T%agent\test_vision.py" || exit /b 1)
echo ALL TESTS OK
