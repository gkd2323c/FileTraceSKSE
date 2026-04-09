@echo off
REM ========================================
REM FileTraceSKSE Build Script
REM ========================================
REM Dependencies (CommonLibSSE-NG) are auto-downloaded via CMake FetchContent.
REM Just run this script from a VS Developer Command Prompt.

setlocal enabledelayedexpansion

set "BUILD_DIR=build"
set "CONFIG=Release"

REM Parse arguments
:parse_args
if "%~1"=="" goto :end_parse_args
if /i "%~1"=="--debug" (
    set "CONFIG=Debug"
)
if /i "%~1"=="--clean" (
    if exist "%BUILD_DIR%" (
        echo Cleaning build directory...
        rmdir /s /q "%BUILD_DIR%"
    )
)
shift
goto :parse_args
:end_parse_args

echo ========================================
echo Building FileTraceSKSE
echo Configuration: %CONFIG%
echo ========================================

REM Configure CMake (FetchContent downloads CommonLibSSE-NG automatically)
echo Configuring... (first build may take a few minutes to download dependencies)
cmake -B "%BUILD_DIR%" -S .

if errorlevel 1 (
    echo.
    echo ERROR: CMake configuration failed
    echo.
    echo Troubleshooting:
    echo   1. Make sure MSVC Build Tools are installed
    echo   2. Run this from "x64 Native Tools Command Prompt for VS 2022"
    echo   3. Check that you have internet access (needed for first build)
    exit /b 1
)

REM Build
echo.
echo Building...
cmake --build "%BUILD_DIR%" --config %CONFIG%

if errorlevel 1 (
    echo.
    echo ERROR: Build failed
    echo.
    echo Check the error messages above for details
    exit /b 1
)

echo.
echo ========================================
echo Build successful!
echo.
echo Output: %BUILD_DIR%\%CONFIG%\FileTraceSKSE.dll
echo.
echo To install:
echo   1. Copy the DLL to: ^<Skyrim^>\Data\SKSE\Plugins\
echo   2. Launch Skyrim with SKSE
echo ========================================
