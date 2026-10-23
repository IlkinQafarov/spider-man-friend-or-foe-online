@echo off
rem Own-code self-test of the proxy load chain. Builds a fake game (static d3d9
rem import) and a fake native-probe.dll into a NEW directory, copies the real proxy
rem d3d9.dll + d3d9_chain.dll beside them, and runs the fake game. A pass proves the
rem proxy loads the probe before entry and forwards d3d9 to the real library.
rem No real game, Steam, account or network is involved.
rem
rem Usage: build_selftest.cmd <new-output-dir>
setlocal
if "%~1"=="" (echo Usage: build_selftest.cmd ^<new-output-dir^> & exit /b 2)
set OUT=%~1
if exist "%OUT%" (echo Output dir must be NEW: %OUT% & exit /b 2)

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"

if not exist d3d9.dll (echo Build the proxy first: run build.cmd & exit /b 1)
if not exist d3d9_chain.dll (echo Missing d3d9_chain.dll; run build.cmd & exit /b 1)

mkdir "%OUT%" || exit /b 1

cl /nologo /W4 /O2 /MT /EHsc /LD selftest_probe.cpp /link /OUT:"%OUT%\native-probe.dll"
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc selftest_game.cpp /link d3d9.lib /OUT:"%OUT%\selftest-game.exe"
if errorlevel 1 exit /b 1

copy /y d3d9.dll "%OUT%\d3d9.dll" >nul
copy /y d3d9_chain.dll "%OUT%\d3d9_chain.dll" >nul

set COOP_PROXY_SELFTEST_MARKER=%OUT%\probe-loaded.marker
if exist "%COOP_PROXY_SELFTEST_MARKER%" del "%COOP_PROXY_SELFTEST_MARKER%"

echo.
echo === running proxy self-test ===
"%OUT%\selftest-game.exe"
set RC=%errorlevel%
echo === exit code %RC% ===
exit /b %RC%
