@echo off
rem build.cmd - convenience wrapper around build.ps1 (passes all arguments).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %errorlevel%
