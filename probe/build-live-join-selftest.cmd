@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
set "LIVE_JOIN_BUILD=%TEMP%\fof-live-join-selftest-%RANDOM%-%RANDOM%"
if exist "%LIVE_JOIN_BUILD%" exit /b 2
mkdir "%LIVE_JOIN_BUILD%"
if errorlevel 1 exit /b 1
pushd "%LIVE_JOIN_BUILD%"
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 /I"%~dp0." "%~dp0live_join_journal_selftest.cpp" /Fe:live-join-selftest.exe
if errorlevel 1 goto failed
live-join-selftest.exe
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
