@echo off
REM Builds and runs the offline test harness (JSON codec + TCP transport).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0.."
if not exist "build\tests" mkdir "build\tests"

cl /nologo /EHsc /std:c++latest /MT /W3 /DWIN32 /DNDEBUG /D_CONSOLE /D_CRT_SECURE_NO_WARNINGS /DYYTK_DEFINE_INTERNAL /I include /I src /Fo:build\tests\ /Fe:build\tests\harness.exe tests\harness.cpp src\Json.cpp src\Sanitize.cpp src\Net.cpp src\Steam.cpp src\ProbeJournal.cpp ws2_32.lib user32.lib
if errorlevel 1 (
    echo [tests] ERROR: build failed.
    exit /b 1
)

echo.
build\tests\harness.exe
endlocal
