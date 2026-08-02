@echo off
REM ---------------------------------------------------------------------------
REM Installs the mod into the game's mods\Aurie\ folder. Double-click me.
REM
REM   install.bat                                  find the game and install
REM   install.bat "D:\...\How Many Dudes"          install into a specific folder
REM   install.bat /detect                          report only, change nothing
REM
REM The real work is in tools\install.ps1 - Steam's default install path
REM contains both spaces and parentheses, which batch handles badly.
REM ---------------------------------------------------------------------------
setlocal

if /i "%~1"=="/detect" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install.ps1" -Detect
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install.ps1" -GameDir "%~1"
)

set "RESULT=%ERRORLEVEL%"

REM Keep the window open when launched from Explorer rather than a terminal.
if "%~1"=="" pause

endlocal & exit /b %RESULT%
