@echo off
setlocal
rem Player edition: no automated connection test (see PLAYER in Launcher.cs).
if not exist "%~dp0player" mkdir "%~dp0player"
"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /codepage:65001 /warn:4 /warnaserror /define:PLAYER /target:winexe /platform:x64 /optimize+ /out:"%~dp0player\fof-private-launcher.exe" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll /reference:System.Web.Extensions.dll "%~dp0Launcher.cs"
exit /b %errorlevel%
