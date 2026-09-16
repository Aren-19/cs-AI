@echo off
rem Open the local replay viewer (starts it if needed).
title CsAI Viewer
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0webun.ps1"
