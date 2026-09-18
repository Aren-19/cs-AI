@echo off
rem CsAI control panel - double-click to open.
rem Started through a hidden console so the panel is the only window on screen.
cd /d "%~dp0"
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tools\panel_gui.ps1"
