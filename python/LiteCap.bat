@echo off
REM ============================================================
REM  LiteCap - minimal offline screen recorder
REM  Launches the app with the isolated Python environment.
REM  No installer, no service, no network.
REM ============================================================
setlocal
set "HERE=%~dp0"
set "VENV=C:\Users\Administrator\.workbuddy-ai\binaries\python\envs\default"

if exist "%VENV%\Scripts\pythonw.exe" (
  start "" "%VENV%\Scripts\pythonw.exe" "%HERE%LiteCap.py" %*
) else (
  where pythonw >nul 2>&1
  if %errorlevel%==0 (
    start "" pythonw "%HERE%LiteCap.py" %*
  ) else (
    echo Python was not found. Install Python 3.10+ or fix the VENV path in this file.
    pause
  )
)
