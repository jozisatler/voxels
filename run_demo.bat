@echo off
set PANDA_PRC_DIR=C:\Panda3D-1.10.16-x64\etc
echo Building Spinning Cube...
cd build
cmake --build . --config Release
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build failed!
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo Starting Spinning Cube...
cd Release
SpinningCube.exe
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Application exited with error.
    pause
)
