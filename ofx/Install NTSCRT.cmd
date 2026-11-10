@echo off
rem Double-click to install the NTSCRT OpenFX plugin for DaVinci Resolve (uses the bundle in this folder).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
pause
