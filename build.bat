@echo off
REM ---------------------------------------------------------------------------
REM Build HowManyDudesMultiplayer.dll (YYToolkit / Aurie plugin, x64).
REM
REM PLAYERS DO NOT NEED THIS. Download the DLL from the Releases page and run
REM install.bat. This script is for developers building from source.
REM
REM No CMake dependency - drives cl.exe directly through the VS environment.
REM All output lands in .\build\.
REM
REM NOTE: this file must be saved with CRLF line endings. cmd.exe misparses a
REM batch file with bare LF endings - @echo off silently fails to take effect
REM and parenthesised if-blocks break. .gitattributes pins this.
REM ---------------------------------------------------------------------------
setlocal

REM Locate the C++ toolchain. vswhere.exe ships with every VS 2017+ installer
REM and finds any edition - BuildTools, Community, Professional, Enterprise -
REM for any year. Hardcoding one path only ever works on the machine it was
REM written on.
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)

REM Fall back to the usual install locations if vswhere is absent or found
REM nothing with the C++ workload.
if not defined VCVARS (
    for %%e in (BuildTools Community Professional Enterprise) do (
        for %%y in (2022 2019) do (
            if not defined VCVARS (
                if exist "%ProgramFiles%\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat"
                if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat"
            )
        )
    )
)

if not defined VCVARS (
    echo [build] ERROR: no Visual Studio C++ toolchain found.
    echo.
    echo   This script needs the MSVC compiler. Install either:
    echo     - Visual Studio Build Tools 2022, or
    echo     - Visual Studio 2022 Community
    echo   and tick the "Desktop development with C++" workload.
    echo.
    echo   https://visualstudio.microsoft.com/downloads/
    echo.
    echo   If you only want to PLAY, you do not need any of this - download
    echo   HowManyDudesMultiplayer.dll from the Releases page instead.
    exit /b 1
)

echo [build] toolchain: "%VCVARS%"
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [build] ERROR: vcvars64.bat failed to initialise the environment.
    exit /b 1
)

cd /d "%~dp0"
if not exist "build" mkdir "build"

del /q build\*.obj >nul 2>&1

set "SOURCES=src\ModuleMain.cpp src\GameBridge.cpp src\GameHooks.cpp src\Json.cpp src\Sanitize.cpp src\Roster.cpp src\Net.cpp src\Steam.cpp src\RunState.cpp src\Ui.cpp src\Probe.cpp src\ProbeJournal.cpp src\Match.cpp include\YYToolkit\YYTK_Shared_Types.cpp"

REM /d1trimfile strips this directory from every __FILE__ the compiler bakes in.
REM MSVC records headers by absolute path, so without it the build machine's
REM directory - and therefore its user account name - ships inside the DLL.
REM The project root is quoted with a doubled trailing backslash: the path
REM contains spaces, and a single trailing backslash would escape the quote.
set "PROJECT_ROOT=%~dp0"
set "PROJECT_ROOT=%PROJECT_ROOT:~0,-1%"

set "CLFLAGS=/nologo /c /EHsc /std:c++latest /O2 /MT /W3 /DWIN32 /DNDEBUG /D_CONSOLE /D_CRT_SECURE_NO_WARNINGS /DYYTK_DEFINE_INTERNAL /d1trimfile:"%PROJECT_ROOT%\\" /I include /I src /Fo:build\"

echo [build] compiling...
cl %CLFLAGS% %SOURCES%
if errorlevel 1 (
    echo [build] ERROR: compilation failed.
    exit /b 1
)

echo [build] linking...
link /nologo /DLL /OUT:build\HowManyDudesMultiplayer.dll build\*.obj ws2_32.lib user32.lib
if errorlevel 1 (
    echo [build] ERROR: link failed.
    exit /b 1
)

echo [build] OK -^> build\HowManyDudesMultiplayer.dll
endlocal
