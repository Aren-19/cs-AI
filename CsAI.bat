@echo off
rem Opens the CsAI panel. The first run builds CsAI.exe, which opens it with no console at all.
rem With arguments it runs a command instead: CsAI.bat teach <map>, status, start, stop.
cd /d "%~dp0"
if not "%~1"=="" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\csai.ps1" %*
    exit /b %errorlevel%
)
if not exist "%~dp0CsAI.exe" (
    "%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /out:"%~dp0CsAI.exe" "%~dp0tools\launcher.cs" >nul 2>&1
)
if exist "%~dp0CsAI.exe" (
    start "" "%~dp0CsAI.exe"
) else (
    start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tools\panel_gui.ps1"
)
