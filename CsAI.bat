@echo off
rem CsAI control panel - double-click to open.
rem Opens the panel and starts training from it. One window, no consoles.
cd /d "%~dp0"
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tools\panel_gui.ps1" -Start "main,windup"
