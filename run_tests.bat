:: Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/
::
:: Runs the CTest suite of an already configured build. The conan runtime environment
:: (Qt, ffmpeg, ...) is activated first, because unit tests resolve their DLLs from the
:: PATH that is present at test-run time.
::
:: Usage: run_tests.bat [ctest arguments...]

@setlocal %= Reset errorlevel and prohibit changing env vars of the parent shell. =%

@set SOURCE_DIR_WITH_BACKSLASH=%~dp0
@set SOURCE_DIR=%SOURCE_DIR_WITH_BACKSLASH:~0,-1%
@set BUILD_DIR=%SOURCE_DIR%-build

:: Initialize the MSVC environment to match the one used for building.
set NX_FORCED_VC_TOOLSET_VERSION=14.44.35207
call "%SOURCE_DIR%\build_utils\msvc\call_vcvars64.bat" || @goto :exit

:: Activate the conan runtime environment generated during the CMake configure step.
if exist "%BUILD_DIR%\activate_run.bat" (
    call "%BUILD_DIR%\activate_run.bat"
) else (
    echo [run_tests] WARNING: "%BUILD_DIR%\activate_run.bat" was not found.
    echo [run_tests] Configure the build first so the conan runtime environment is generated.
)

if "%~1"=="" (
    ctest --test-dir "%BUILD_DIR%" --output-on-failure
) else (
    ctest --test-dir "%BUILD_DIR%" %*
)

:exit
    @exit /b %ERRORLEVEL% %= Needed for a proper cmd.exe exit status. =%
