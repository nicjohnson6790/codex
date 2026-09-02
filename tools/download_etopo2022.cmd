@echo off
setlocal
set "REPO_ROOT=%~dp0.."
for %%I in ("%REPO_ROOT%") do set "REPO_ROOT=%%~fI"
set "SOURCE_DIR=%REPO_ROOT%\assets\source\etopo2022"
set "TARGET=%SOURCE_DIR%\ETOPO_2022_v1_60s_N90W180_surface.tif"
set "PART=%TARGET%.part"
set "URL=https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/60s_surface_elev_gtif/ETOPO_2022_v1_60s_N90W180_surface.tif"
set "FORCE=0"

if /I "%~1"=="--force" set "FORCE=1"
if not "%~1"=="" if /I not "%~1"=="--force" (
    echo Usage: tools\download_etopo2022.cmd [--force]
    exit /b 2
)

where curl.exe >nul 2>nul
if errorlevel 1 (
    echo Error: Windows curl.exe was not found on PATH.
    exit /b 3
)

if exist "%TARGET%" if "%FORCE%"=="0" (
    echo ETOPO source already exists: "%TARGET%"
    echo Use --force to download it again.
    exit /b 0
)

if not exist "%SOURCE_DIR%" mkdir "%SOURCE_DIR%"
if errorlevel 1 (
    echo Error: could not create "%SOURCE_DIR%".
    exit /b 4
)
if exist "%PART%" del /q "%PART%"

echo Downloading ETOPO 2022 60 arc-second surface elevation GeoTIFF...
curl.exe --fail --location --retry 4 --retry-delay 5 --output "%PART%" "%URL%"
if errorlevel 1 (
    echo Error: ETOPO download failed. Partial file remains at "%PART%".
    exit /b 5
)
if not exist "%PART%" (
    echo Error: curl reported success but did not create "%PART%".
    exit /b 6
)
if exist "%TARGET%" del /q "%TARGET%"
move /y "%PART%" "%TARGET%" >nul
if errorlevel 1 (
    echo Error: could not finalize the download as "%TARGET%".
    exit /b 7
)
echo Downloaded: "%TARGET%"
exit /b 0
