@echo off
REM ---------------------------------------------------------------------------
REM Build HowManyDudesMultiplayer.dll (YYToolkit / Aurie plugin, x64).
REM No CMake dependency - drives cl.exe directly through the VS BuildTools
REM environment. All output lands in .\build\.
REM
REM The project path contains spaces, so everything below runs relative to the
REM project root rather than passing absolute paths on the command line.
REM ---------------------------------------------------------------------------
setlocal

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [build] ERROR: vcvars64.bat not found at "%VCVARS%".
    exit /b 1
)

call "%VCVARS%" >nul 2>&1

cd /d "%~dp0"
if not exist "build" mkdir "build"

del /q build\*.obj >nul 2>&1

set "SOURCES=src\ModuleMain.cpp src\GameBridge.cpp src\Json.cpp src\Sanitize.cpp src\Roster.cpp src\Net.cpp src\Steam.cpp src\Match.cpp include\YYToolkit\YYTK_Shared_Types.cpp"

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
