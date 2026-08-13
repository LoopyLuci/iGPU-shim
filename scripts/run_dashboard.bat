@echo off
echo Starting Synapse iGPU Dashboard (Rust Axum)...
cd /d %~dp0..
cd rust
cargo run -p igpu_ml_dashboard
pause
