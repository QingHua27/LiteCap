@echo off
REM ============================================================
REM  LiteCap - offline self-check
REM  Scans the source for any network-related import and reports
REM  whether the program is capable of making network requests.
REM ============================================================
setlocal
set "HERE=%~dp0"
set "VENV=C:\Users\Administrator\.workbuddy-ai\binaries\python\envs\default"
if exist "%VENV%\Scripts\python.exe" (
  "%VENV%\Scripts\python.exe" "%HERE%LiteCap.py" --self-check
) else (
  python "%HERE%LiteCap.py" --self-check
)
echo.
pause
