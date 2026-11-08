@echo off
rem P13001L auto-connect console launcher.
rem   p13.cmd                 -> wait for tablet, connect, interactive prompt
rem   p13.cmd --command "ver" -> one-shot
rem   p13.cmd --script FILE   -> run one command per line
rem   p13.cmd --list          -> list COM devices
setlocal
set PYTHONDONTWRITEBYTECODE=1
python "%~dp0p13.py" %*
exit /b %errorlevel%
