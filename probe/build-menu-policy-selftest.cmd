@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist menu-policy-selftest-obj mkdir menu-policy-selftest-obj
cd menu-policy-selftest-obj
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 ..\mission_menu_policy_selftest.cpp /Fonormal.obj /Fe:normal.exe
if errorlevel 1 exit /b 1
normal.exe
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 /DFOF_TEST_ALLOW_PARTNER_MENU ..\mission_menu_policy_selftest.cpp /Fomutant.obj /Fe:mutant.exe
if errorlevel 1 exit /b 1
mutant.exe
if not "%errorlevel%"=="1" exit /b 2
echo MISSION_MENU_POLICY_MUTATION_CAUGHT
exit /b 0
