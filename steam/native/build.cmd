@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0"
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE main.cpp relay.cpp steam.cpp tests.cpp /Fe:native-relay.exe /link /NOLOGO /DYNAMICBASE /NXCOMPAT ws2_32.lib bcrypt.lib winmm.lib
set buildResult=%errorlevel%
popd
exit /b %buildResult%
