@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem zEngine Debug build helper.
rem The first launch requests administrator approval through the normal UAC prompt.
rem It does not disable Microsoft Defender or change any system security settings.

if /I "%~1"=="__elevated" goto :build

rem fltmc succeeds only when this process already has administrator rights.
fltmc >nul 2>&1
if not errorlevel 1 goto :build

echo Requesting administrator approval for the build...
powershell.exe -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '__elevated' -Verb RunAs"
if errorlevel 1 (
    echo The administrator prompt could not be started or was cancelled.
    pause
    exit /b 1
)
exit /b 0

:build
set "ROOT=%~dp0"
set "BUILD=%ROOT%builds\debug"
set "CMAKE=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not exist "%CMAKE%" (
    echo Could not find Visual Studio CMake at:
    echo %CMAKE%
    pause
    exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"
if errorlevel 1 (
    echo Could not create the debug build directory:
    echo %BUILD%
    pause
    exit /b 1
)

echo.
echo Configuring zEngine Debug build...
"%CMAKE%" -S "%ROOT%" -B "%BUILD%" -A x64
if errorlevel 1 (
    echo.
    echo CMake configuration failed.
    pause
    exit /b 1
)

echo.
echo Building zEngine...
"%CMAKE%" --build "%BUILD%" --config Debug --target zEngine
if errorlevel 1 (
    echo.
    echo Build failed.
    pause
    exit /b 1
)

echo.
echo Build complete:
echo %BUILD%\Debug\zEngine.exe
pause
exit /b 0
