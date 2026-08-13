@echo off
echo Starting Synapse iGPU Dashboard (Rust Axum)...
cd /d %~dp0..
cargo run -p igpu_ml_dashboard
pause
