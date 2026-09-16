@echo off
rem CsAI control panel - double-click to open.
title CsAI
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\panel.ps1"
