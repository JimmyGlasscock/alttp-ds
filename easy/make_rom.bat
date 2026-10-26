@echo off
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  py -3 make_rom.py %*
) else (
  python make_rom.py %*
)

echo.
pause
