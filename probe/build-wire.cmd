@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 input_wire.cpp wire_selftest.cpp /Fe:native-wire-selftest.exe
exit /b %errorlevel%
