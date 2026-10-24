@echo off
rem Player edition of the probe: bots, scripted scenarios and fault fixtures cannot be switched on (FOF_PLAYER_BUILD).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist player mkdir player
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 /LD /DFOF_PLAYER_BUILD probe.cpp input_wire.cpp /Fo:player\ /link /OUT:player\native-probe.dll user32.lib gdi32.lib shell32.lib advapi32.lib shlwapi.lib
exit /b %errorlevel%
