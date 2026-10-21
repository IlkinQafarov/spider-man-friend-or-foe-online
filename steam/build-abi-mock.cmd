@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0"
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 /LD api_abi_mock.cpp /Fe:api_abi_mock.dll /link /NOLOGO
set buildResult=%errorlevel%
popd
exit /b %buildResult%
