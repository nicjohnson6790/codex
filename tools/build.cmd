@echo off
set "BUILD_TYPE=%~1"
if not defined BUILD_TYPE set "BUILD_TYPE=Release"
if /I "%BUILD_TYPE%"=="Assets" goto :build_assets

set "BUILD_DIR=build\%BUILD_TYPE%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%BUILD_DIR%\build.ninja" call "%~dp0configure.cmd" %BUILD_TYPE%
if errorlevel 1 exit /b %errorlevel%
call :ensure_ninja_cache "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%
call :setup_vs_tools
if errorlevel 1 exit /b %errorlevel%

"C:\Program Files\CMake\bin\cmake.exe" --build "%BUILD_DIR%" --parallel %NUMBER_OF_PROCESSORS%
exit /b %errorlevel%

:build_assets
set "BUILD_DIR=build\Assets"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%BUILD_DIR%\build.ninja" call "%~dp0configure.cmd" Assets
if errorlevel 1 exit /b %errorlevel%
call :ensure_ninja_cache "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%
call :setup_vs_tools
if errorlevel 1 exit /b %errorlevel%

"C:\Program Files\CMake\bin\cmake.exe" --build "%BUILD_DIR%" --parallel %NUMBER_OF_PROCESSORS%
if errorlevel 1 exit /b %errorlevel%

"%BUILD_DIR%\converter.exe" skybox
if errorlevel 1 exit /b %errorlevel%
"%BUILD_DIR%\converter.exe" pinetreepack
if errorlevel 1 exit /b %errorlevel%
"%BUILD_DIR%\converter.exe" pbr
exit /b %errorlevel%

:setup_vs_tools
set "VS_ROOT="
if exist "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" set "VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VS_ROOT if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" set "VS_ROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not defined VS_ROOT (
    echo Visual Studio developer tools were not found.
    exit /b 1
)

call "%VS_ROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

if exist "%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\mingw64\bin\git.exe" set "PATH=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\mingw64\bin;%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd;%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\mingw64\libexec\git-core;%PATH%"
if exist "%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "PATH=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
for /d %%D in ("%VS_ROOT%\VC\Tools\MSVC\*") do set "MSVC_VERSION=%%~nxD"
if defined MSVC_VERSION set "PATH=%VS_ROOT%\VC\Tools\MSVC\%MSVC_VERSION%\bin\Hostx64\x64;%PATH%"
set "VULKAN_SDK=C:\VulkanSDK\1.4.341.1"

where ninja.exe >nul 2>nul
if errorlevel 1 (
    echo Ninja was not found on PATH.
    exit /b 1
)
exit /b 0

:ensure_ninja_cache
set "CACHE_DIR=%~1"
if exist "%CACHE_DIR%\CMakeCache.txt" (
    findstr /B /C:"CMAKE_GENERATOR:INTERNAL=Ninja" "%CACHE_DIR%\CMakeCache.txt" >nul
    if errorlevel 1 (
        echo Existing CMake cache in %CACHE_DIR% uses a different generator.
        echo Remove %CACHE_DIR% or configure a clean build directory before switching to Ninja.
        exit /b 1
    )
)
exit /b 0
