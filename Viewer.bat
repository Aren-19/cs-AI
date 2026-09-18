@echo off
rem Open the local replay viewer (starts it if needed).
cd /d "%~dp0"
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0webun.ps1"
