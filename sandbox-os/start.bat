@echo off
rem Boot SandboxOS and open the console.
cd /d "%~dp0"

where py >nul 2>nul
if %errorlevel%==0 (
  set PY=py
) else (
  set PY=python
)

echo Booting SandboxOS...
start "" http://127.0.0.1:8787
%PY% run.py %*
if errorlevel 1 (
  echo.
  echo SandboxOS exited with an error. If Python is missing, install it from
  echo https://www.python.org/downloads/ and run this file again.
  pause
)
