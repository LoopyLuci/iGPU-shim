@echo off
echo Starting Synapse iGPU Dashboard...
cd /d %~dp0dashboard
python -m uvicorn app:app --host 127.0.0.1 --port 8765
pause
