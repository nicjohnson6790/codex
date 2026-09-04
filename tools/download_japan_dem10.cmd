@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0download_japan_dem10.ps1" %*
exit /b %ERRORLEVEL%
