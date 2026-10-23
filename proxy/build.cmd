@echo off
rem Build the d3d9 proxy for the player / closed-test load path.
rem Generates d3d9_forwards.h forwarders from THIS machine's 32-bit system d3d9, builds
rem d3d9.dll, and drops d3d9_chain.dll (a local copy of the real d3d9 that the
rem forwarders target) beside it. No game files are read or written.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"

python -I make_def.py --d3d9 C:\Windows\SysWOW64\d3d9.dll --out d3d9_forwards.h --chain-copy .
if errorlevel 1 exit /b 1

cl /nologo /W4 /O2 /MT /EHsc /D_CRT_SECURE_NO_WARNINGS /LD proxy.cpp /link /OUT:d3d9.dll
if errorlevel 1 exit /b 1

echo.
echo Built d3d9.dll and d3d9_chain.dll in %~dp0
exit /b 0
