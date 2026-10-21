@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist wire-selftest-obj mkdir wire-selftest-obj
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 wire_stop_selftest.cpp input_wire.cpp /Fowire-selftest-obj\ /link /OUT:wire-stop-selftest.exe
exit /b %errorlevel%
