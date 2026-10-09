:: Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

@setlocal %= Reset errorlevel and prohibit changing env vars of the parent shell. =%

@set SOURCE_DIR_WITH_BACKSLASH=%~dp0
@set SOURCE_DIR=%SOURCE_DIR_WITH_BACKSLASH:~0,-1%

:: Prioritize pip-installed tools (cmake, ninja, conan) over system ones.
@set PATH=C:\Users\liusu\AppData\Roaming\Python\Python313\Scripts;%PATH%

:: Initialize the MSVC environment. The previously pinned 14.40 toolset is no longer usable on
:: this machine (its bin/ and include/ were removed), so we force the installed MSVC 14.44.
set NX_FORCED_VC_TOOLSET_VERSION=14.44.35207
call %SOURCE_DIR%\build_utils\msvc\call_vcvars64.bat || @goto :exit

:: nvcc 12.6 can find its own Windows SDK headers and the rest of the MSVC toolset
:: by walking up from the cl.exe path, so we keep VCToolsInstallDir pointed at the
:: real (space-containing) install. CMake/nvcc handle quoted -ccbin paths fine.

:: Enable Nvidia hardware decoder via CUDA 12.8 (v12.6 is no longer installed). We override
:: CUDAToolkit_ROOT so CMake picks up the v12.8 toolkit instead of any other leftover install.
@set NX_CUDA_OPT=-DwithNvidiaDecoder=ON
if exist "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8" (
    @set NX_CUDA_TOOLKIT_DIR=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8
)

:: Expose the real cl.exe path to CMake. We deliberately do NOT create a junction
:: because nvcc needs the real VCToolsInstallDir parent to discover Windows SDK
:: headers (the junction hides the rest of the Visual Studio tree).
if not "%VCToolsInstallDir%" == "" (
    @set NX_MSVC_CL_EXE=%VCToolsInstallDir%bin\Hostx64\x64\cl.exe
)

:: Make the build dir at the same level as the parent dir of this script, suffixed with "-build".
@set BUILD_DIR=%SOURCE_DIR%-build

@if exist "%BUILD_DIR%\CMakeCache.txt" goto :skip_generation
    @mkdir "%BUILD_DIR%" 2>NUL

    :: Ensure that only the currently supplied CMake arguments are in effect.
    del "%BUILD_DIR%\CMakeCache.txt" 2>NUL

    :: Use the Ninja generator to avoid using CMake compiler search heuristics for MSBuild. Also,
    :: specify the compiler explicitly to avoid potential clashes with gcc if ran from a
    :: Cygwin/MinGW shell.
    set GENERATOR_OPTIONS=-GNinja -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe
    cmake "%SOURCE_DIR%" %GENERATOR_OPTIONS% -B "%BUILD_DIR%" %NX_CUDA_OPT% %* || @goto :exit
:skip_generation

cmake --build "%BUILD_DIR%"

:exit
    @exit /b %ERRORLEVEL% %= Needed for a proper cmd.exe exit status. =%
