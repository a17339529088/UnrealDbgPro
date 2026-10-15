@echo off
setlocal DisableDelayedExpansion
rem ASCII wrapper: paths containing ! remain intact. --no-pause is for CI.
set "RC=0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0UnrealDbgProNative\BuildAndVerifyGui.ps1"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" echo [FAIL] Build or validation failed. Exit code: %RC%
if /I not "%~1"=="--no-pause" pause
exit /b %RC%
