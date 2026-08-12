@echo off
rem Smoke-test the dashboard: starts server, hits endpoints, stops server.
echo [1/3] Launching dashboard...
cd /d %~dp0dashboard
start "synapse-dashboard" /B python -m uvicorn app:app --host 127.0.0.1 --port 8765
timeout /t 2 /nobreak >nul

echo [2/3] Checking endpoints...
curl -s http://127.0.0.1:8765/api/health >nul
if errorlevel 1 (
    echo FAIL: /api/health unreachable
    goto :stop
)
curl -s http://127.0.0.1:8765/ | findstr /C:"Synapse iGPU Dashboard" >nul
if errorlevel 1 (
    echo FAIL: index page missing brand
    goto :stop
)
echo PASS: dashboard smoke test

:stop
echo [3/3] Stopping dashboard...
for /f "tokens=2" %%i in ('tasklist /fi "imagename eq python.exe" /fo csv ^| findstr /i "python"') do (
    echo Killing PID %%i
    taskkill /pid %%i /f >nul 2>&1
)
pause
