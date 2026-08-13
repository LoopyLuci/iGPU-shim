@echo off
rem Smoke-test the Rust dashboard: starts server, hits endpoints, stops server.
echo [1/3] Launching dashboard...
cd /d %~dp0..
start "synapse-dashboard" /B cmd.exe /c "cd rust && cargo run -p igpu_ml_dashboard"
timeout /t 3 /nobreak >nul

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
curl -s -X POST http://127.0.0.1:8765/api/ml/infer -H "Content-Type: application/json" -d "{\"shader_complexity_norm\":0.1,\"vertex_count_log_norm\":0.2,\"draw_call_rate_norm\":0.3,\"cache_hit_rate\":0.4,\"dvfs_headroom\":0.5,\"thermal_headroom\":0.6,\"predictor_accuracy\":0.7,\"is_compute_dispatch\":0.0}" | findstr /C:"dynamic-tile-transformer" >nul
if errorlevel 1 (
    echo FAIL: /api/ml/infer missing model name
    goto :stop
)
echo PASS: dashboard smoke test

:stop
echo [3/3] Stopping dashboard...
for /f "tokens=2" %%i in ('tasklist /fi "imagename eq igpu_ml_dashboard.exe" /fo csv ^| findstr /i "igpu_ml_dashboard"') do (
    echo Killing PID %%i
    taskkill /pid %%i /f >nul 2>&1
)
pause
