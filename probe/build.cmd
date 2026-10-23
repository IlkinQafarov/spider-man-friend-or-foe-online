@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 /LD probe.cpp input_wire.cpp /link /OUT:native-probe.dll user32.lib gdi32.lib shell32.lib advapi32.lib shlwapi.lib
if errorlevel 1 exit /b 1
call "%~dp0..\proxy\build.cmd"
if errorlevel 1 exit /b 1
copy /y "%~dp0..\proxy\d3d9.dll" "%~dp0d3d9.dll" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /W4 /O2 /MT /EHsc launcher.cpp /link /OUT:native-launcher.exe bcrypt.lib
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc selftest.cpp /link /BASE:0x400000 /DYNAMICBASE:NO /OUT:native-selftest.exe shell32.lib advapi32.lib
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 progress_selftest.cpp /link /OUT:native-progress-selftest.exe
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 mode_selftest.cpp /link /OUT:native-mode-selftest.exe
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 input_key_selftest.cpp /link /OUT:native-input-key-selftest.exe
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 campaign_phase_selftest.cpp /link /OUT:native-campaign-phase-selftest.exe
exit /b %errorlevel%
