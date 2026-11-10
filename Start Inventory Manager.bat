@echo off
title Inventory Manager
cd /d "%~dp0"

where python >nul 2>&1
if not errorlevel 1 (
  python server.py %*
  goto done
)
where py >nul 2>&1
if not errorlevel 1 (
  py -3 server.py %*
  goto done
)
echo.
echo   Python 3 was not found on this computer.
echo   Install it from https://www.python.org/downloads/ and run this file again.
echo.

:done
pause
