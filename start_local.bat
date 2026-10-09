@echo off
chcp 65001 >nul
echo Starting Nx MetaVMS in local mode...
echo.
set PATH=E:\GitRepo\nx_open-build\.conan_short\48dc6f\1\bin;%PATH%
cd /d E:\GitRepo\nx_open-build\bin
start "NxMeta" "Nx MetaVMS.exe" --local-mode
pause
