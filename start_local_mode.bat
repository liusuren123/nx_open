@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion

REM Set Qt library path
set "PATH=E:\GitRepo\nx_open-build\.conan_short\48dc6f\1\bin;%PATH%"

REM Set working directory
set "WORKDIR=E:\GitRepo\nx_open-build\bin"

REM Launch the client with local mode and capture stderr
echo Starting Nx MetaVMS in local mode...
echo.
echo If the client crashes, check the log at:
echo C:\Users\liusu\AppData\Local\Network Optix\Network Optix Nx MetaVMS Client\log\client_log.log
echo.

start "Nx MetaVMS" cmd /c "cd /d "%WORKDIR%" ^&^& "Nx MetaVMS.exe" --local-mode 2^>^&1 ^& pause"
