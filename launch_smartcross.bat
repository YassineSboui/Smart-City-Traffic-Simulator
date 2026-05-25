@echo off
setlocal

cd /d "%~dp0"

where wsl.exe >nul 2>nul
if errorlevel 1 (
    echo WSL is not installed or not available in PATH.
    echo Please install WSL, then run this launcher again.
    pause
    exit /b 1
)

for /f "usebackq delims=" %%I in (`wsl.exe wslpath -a "%CD%"`) do set "WSL_DIR=%%I"

if not defined WSL_DIR (
    echo Could not convert the project path for WSL.
    pause
    exit /b 1
)

echo Building and launching SmartCross...
echo Project: %CD%
echo.

wsl.exe --cd "%WSL_DIR%" --exec /bin/bash -lc "make && ./smartcross"

if errorlevel 1 (
    echo.
    echo SmartCross failed to launch. Check the messages above.
    pause
    exit /b 1
)

endlocal
