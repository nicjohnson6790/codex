@echo off
set "BUILD_TYPE=%~1"
if not defined BUILD_TYPE set "BUILD_TYPE=Release"
if /I "%BUILD_TYPE%"=="Assets" goto :build_assets

set "BUILD_DIR=build\%BUILD_TYPE%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
call :ensure_ninja_cache "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%
if "%CMAKE_CACHE_STALE%"=="1" call "%~dp0configure.cmd" %BUILD_TYPE%
if errorlevel 1 exit /b %errorlevel%
if not exist "%BUILD_DIR%\build.ninja" call "%~dp0configure.cmd" %BUILD_TYPE%
if errorlevel 1 exit /b %errorlevel%
call :setup_vs_tools
if errorlevel 1 exit /b %errorlevel%

"C:\Program Files\CMake\bin\cmake.exe" --build "%BUILD_DIR%" --parallel %NUMBER_OF_PROCESSORS%
exit /b %errorlevel%

:build_assets
set "ASSET_CONFIG=%~2"
if not defined ASSET_CONFIG set "ASSET_CONFIG=Release"
if /I not "%ASSET_CONFIG%"=="Debug" if /I not "%ASSET_CONFIG%"=="Release" (
    echo Asset configuration must be Debug or Release.
    echo Usage: tools\build.cmd Assets [Debug^|Release]
    exit /b 1
)
set "BUILD_DIR=build\Assets\%ASSET_CONFIG%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
call :ensure_ninja_cache "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%
if "%CMAKE_CACHE_STALE%"=="1" call "%~dp0configure.cmd" Assets %ASSET_CONFIG%
if errorlevel 1 exit /b %errorlevel%
if not exist "%BUILD_DIR%\build.ninja" call "%~dp0configure.cmd" Assets %ASSET_CONFIG%
if errorlevel 1 exit /b %errorlevel%
findstr /B /C:"CMAKE_BUILD_TYPE:STRING=%ASSET_CONFIG%" "%BUILD_DIR%\CMakeCache.txt" >nul
if errorlevel 1 (
    echo Assets build is not configured for %ASSET_CONFIG%. Reconfiguring.
    call "%~dp0configure.cmd" Assets %ASSET_CONFIG%
)
if errorlevel 1 exit /b %errorlevel%
call :setup_vs_tools
if errorlevel 1 exit /b %errorlevel%

"C:\Program Files\CMake\bin\cmake.exe" --build "%BUILD_DIR%" --parallel %NUMBER_OF_PROCESSORS%
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
set "CMAKE_CACHE_STALE=0"
if exist "%CACHE_DIR%\build.ninja" if not exist "%CACHE_DIR%\CMakeFiles\rules.ninja" (
    echo Incomplete Ninja configuration in %CACHE_DIR%: CMakeFiles\rules.ninja is missing.
    echo Refreshing the CMake cache.
    set "CMAKE_CACHE_STALE=1"
)
if exist "%CACHE_DIR%\CMakeCache.txt" (
    findstr /B /C:"CMAKE_GENERATOR:INTERNAL=Ninja" "%CACHE_DIR%\CMakeCache.txt" >nul
    if errorlevel 1 (
        echo Existing CMake cache in %CACHE_DIR% uses a different generator.
        echo Remove %CACHE_DIR% or configure a clean build directory before switching to Ninja.
        exit /b 1
    )
)
set "CACHED_CXX_COMPILER="
if exist "%CACHE_DIR%\CMakeCache.txt" for /f "tokens=1,* delims==" %%A in ('findstr /B /C:"CMAKE_CXX_COMPILER:FILEPATH=" "%CACHE_DIR%\CMakeCache.txt"') do set "CACHED_CXX_COMPILER=%%B"
if defined CACHED_CXX_COMPILER if not exist "%CACHED_CXX_COMPILER%" (
    echo Cached compiler no longer exists: "%CACHED_CXX_COMPILER%"
    echo Refreshing the CMake cache for the installed Visual Studio toolset.
    set "CMAKE_CACHE_STALE=1"
)
exit /b 0
