@echo off
setlocal
set T=%~dp0
call "%T%build_tests.bat" || exit /b 1
set SC=%TEMP%\s2dev_scenario.jsonl
"%T%test_all.exe" --scenario "%SC%" || exit /b 1
node "%T%test_scenario.js" "%SC%" || exit /b 1
node "%T%test_mcp.js" || exit /b 1
echo ALL TESTS OK
