@echo off
rem Opens the local replay viewer, starting it if needed.
cd /d "%~dp0"
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0web\run.ps1"
