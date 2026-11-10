@echo off
rem Start-menu "Update NTSCRT plug-in": downloads and installs the latest NTSCRT release.
rem Asks for administrator rights once (UAC). Extra arguments go to install.ps1 (e.g. -Version v0.1.0).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
