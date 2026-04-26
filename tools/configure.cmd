@echo off
set "BUILD_TYPE=%~1"
if not defined BUILD_TYPE set "BUILD_TYPE=Release"
set "BUILD_DIR=build\%BUILD_TYPE%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

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
for /d %%D in ("%VS_ROOT%\VC\Tools\MSVC\*") do set "MSVC_VERSION=%%~nxD"
if defined MSVC_VERSION set "PATH=%VS_ROOT%\VC\Tools\MSVC\%MSVC_VERSION%\bin\Hostx64\x64;%PATH%"
set "VULKAN_SDK=C:\VulkanSDK\1.4.341.1"
"C:\Program Files\CMake\bin\cmake.exe" -S . -B "%BUILD_DIR%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
