@echo off
rem build-malitest.cmd - convenience wrapper around tools\build-malitest.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-malitest.ps1" %*
exit /b %errorlevel%
