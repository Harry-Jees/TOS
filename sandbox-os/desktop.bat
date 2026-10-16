@echo off
rem Launch the SandboxOS desktop app (native window, no browser needed).
cd /d "%~dp0"

where py >nul 2>nul
if %errorlevel%==0 (
  set PY=py
) else (
  set PY=python
)

%PY% -u run.py --desktop %*
if errorlevel 1 (
  echo.
  echo SandboxOS exited with an error.
  echo If Python is missing, install Python 3.10+ from https://www.python.org/downloads/
  pause
)
