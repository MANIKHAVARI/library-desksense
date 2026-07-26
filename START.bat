@echo off
setlocal EnableExtensions

rem Safe, portable Windows launcher for the local LibraryDeskSense services.
rem Configure INFLUX_URL, INFLUX_ORG, INFLUX_BUCKET, and INFLUX_TOKEN first.

set "PROJECT=%~dp0"
set "GRAFANA_PORT=3100"

title LibraryDeskSense Launcher
echo.
echo Starting LibraryDeskSense from:
echo %PROJECT%
echo.

for %%V in (INFLUX_URL INFLUX_ORG INFLUX_BUCKET INFLUX_TOKEN) do (
    if not defined %%V (
        echo ERROR: Environment variable %%V is not set.
        echo Follow the Configuration section in README.md, then try again.
        echo.
        pause
        exit /b 1
    )
)

set "PYTHON_COMMAND=python"
if exist "%PROJECT%.venv\Scripts\python.exe" (
    set "PYTHON_COMMAND=%PROJECT%.venv\Scripts\python.exe"
) else (
    where python >nul 2>&1
    if errorlevel 1 (
        echo ERROR: Python was not found.
        echo Install Python or create the .venv described in README.md.
        echo.
        pause
        exit /b 1
    )
)

set "MOSQUITTO_COMMAND="
if exist "C:\Program Files\Mosquitto\mosquitto.exe" (
    set "MOSQUITTO_COMMAND=C:\Program Files\Mosquitto\mosquitto.exe"
) else (
    for /f "delims=" %%I in ('where mosquitto 2^>nul') do (
        if not defined MOSQUITTO_COMMAND set "MOSQUITTO_COMMAND=%%I"
    )
)

if defined MOSQUITTO_COMMAND (
    start "LibraryDeskSense - Mosquitto" "%MOSQUITTO_COMMAND%" -c "%PROJECT%mosquitto.conf" -v
) else (
    echo WARNING: Mosquitto was not found. Start it manually before the backend.
)

set "INFLUX_COMMAND="
for /f "delims=" %%I in ('where influxd 2^>nul') do (
    if not defined INFLUX_COMMAND set "INFLUX_COMMAND=%%I"
)

if defined INFLUX_COMMAND (
    start "LibraryDeskSense - InfluxDB" "%INFLUX_COMMAND%"
) else (
    echo WARNING: influxd was not found on PATH. Start InfluxDB manually if needed.
)

set "GRAFANA_COMMAND="
if exist "C:\Program Files\GrafanaLabs\grafana\bin\grafana.exe" (
    set "GRAFANA_COMMAND=C:\Program Files\GrafanaLabs\grafana\bin\grafana.exe"
) else if exist "C:\Program Files\GrafanaLabs\grafana\bin\grafana-server.exe" (
    set "GRAFANA_COMMAND=C:\Program Files\GrafanaLabs\grafana\bin\grafana-server.exe"
)

if defined GRAFANA_COMMAND (
    set "GF_SERVER_HTTP_PORT=%GRAFANA_PORT%"
    set "GF_PATHS_PROVISIONING=%PROJECT%grafana\provisioning"
    set "GF_DASHBOARDS_DEFAULT_HOME_DASHBOARD_PATH=%PROJECT%grafana\dashboards\library_desksense.json"
    set "GF_AUTH_ANONYMOUS_ENABLED=true"
    set "GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer"
    start "LibraryDeskSense - Grafana" "%GRAFANA_COMMAND%" server --homepath "C:\Program Files\GrafanaLabs\grafana"
) else (
    echo WARNING: Grafana was not found at its standard Windows path.
)

timeout /t 2 /nobreak >nul
start "LibraryDeskSense - Backend" "%PYTHON_COMMAND%" "%PROJECT%library_desksense.py"

timeout /t 5 /nobreak >nul
start "" "http://127.0.0.1:%GRAFANA_PORT%/d/library-desksense/librarydesksense-live-dashboard?orgId=1&refresh=5s"

echo.
echo Local services have been started where their executables were available.
echo Flash and monitor the ESP32 separately with the commands in README.md.
echo.
pause

endlocal
