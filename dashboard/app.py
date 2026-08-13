# Deprecated: use scripts/run_dashboard.bat (Rust backend)
"""
dashboard/app.py
Project Synapse – iGPU Dashboard backend

Exposes:
- GET / -> dashboard single-page frontend
- GET /api/report -> latest report.json content
- GET /api/settings -> user-editable settings
- POST /api/settings -> save settings
- GET /api/wal -> recent WAL entries
- WS /ws -> real-time telemetry push
"""

from __future__ import annotations

import json
import os
import pathlib
import time
from collections import deque
from typing import Any

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

APP_DIR = pathlib.Path(__file__).parent
REPO_ROOT = APP_DIR.parent
REPORT_PATH = REPO_ROOT / "report.json"
SETTINGS_PATH = REPO_ROOT / "dashboard_settings.json"
WAL_PATH = REPO_ROOT / "build_stub" / "Release" / "synapse.wal"

app = FastAPI(title="Synapse iGPU Dashboard")
app.mount("/static", StaticFiles(directory=APP_DIR / "static"), name="static")

# In-memory recent WAL cache for quick polling without parsing the full WAL binary.
_recent_wal: deque[dict[str, Any]] = deque(maxlen=200)


def _read_report() -> dict[str, Any]:
    if REPORT_PATH.exists():
        try:
            return json.loads(REPORT_PATH.read_text(encoding="utf-8"))
        except Exception:
            return {"_error": "invalid report.json"}
    return {"_error": "report.json not found"}


def _default_settings() -> dict[str, Any]:
    return {
        "telemetry": {
            "drawTelemetryEnabled": True,
            "computeTelemetryEnabled": True,
            "walEnabled": True,
            "sampleRateHz": 60,
        },
        "its": {
            "temporalWindowFrames": 3,
            "targetHitRate": 0.89,
            "targetMaxStalls": 15,
        },
        "dvfs": {
            "hysteresisFrames": 5,
            "transitionLockUs": 75,
            "thermalMitigationThreshold": 0.2,
        },
        "power": {
            "targetMilliwattsSaved": 200.0,
            "batteryExtensionFactor": 1.0,
        },
        "display": {
            "theme": "dark",
            "realtimeUpdates": True,
            "chartSmoothing": True,
        },
    }


def _read_settings() -> dict[str, Any]:
    if SETTINGS_PATH.exists():
        try:
            data = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
            if isinstance(data, dict):
                return data
        except Exception:
            pass
    return _default_settings()


def _write_settings(data: dict[str, Any]) -> None:
    SETTINGS_PATH.write_text(json.dumps(data, indent=2), encoding="utf-8")


@app.get("/", response_class=HTMLResponse)
async def index() -> str:
    return (APP_DIR / "templates" / "index.html").read_text(encoding="utf-8")


@app.get("/api/report")
async def api_report() -> JSONResponse:
    return JSONResponse(_read_report())


@app.get("/api/settings")
async def api_get_settings() -> JSONResponse:
    return JSONResponse(_read_settings())


@app.post("/api/settings")
async def api_save_settings(payload: dict[str, Any]) -> JSONResponse:
    if not isinstance(payload, dict):
        return JSONResponse({"_error": "invalid payload"}, status_code=400)
    _write_settings(payload)
    return JSONResponse({"ok": True})


@app.get("/api/wal")
async def api_wal(limit: int = 100) -> JSONResponse:
    entries = list(_recent_wal)[-max(1, min(limit, 500)):]
    return JSONResponse({"entries": entries, "count": len(entries)})


@app.get("/api/health")
async def api_health() -> JSONResponse:
    return JSONResponse({
        "status": "ok",
        "report_exists": REPORT_PATH.exists(),
        "wal_exists": WAL_PATH.exists(),
        "time": time.time(),
    })


@app.websocket("/ws")
async def ws(ws: WebSocket) -> None:
    await ws.accept()
    try:
        await ws.send_json({"type": "hello", "time": time.time()})
        while True:
            msg = await ws.receive_text()
            if msg == "ping":
                await ws.send_json({"type": "pong", "time": time.time()})
            elif msg == "report":
                await ws.send_json({"type": "report", "data": _read_report()})
            elif msg == "settings":
                await ws.send_json({"type": "settings", "data": _read_settings()})
            elif msg == "wal":
                await ws.send_json({"type": "wal", "data": list(_recent_wal)})
            else:
                await ws.send_json({"type": "info", "message": "unknown message"})
    except WebSocketDisconnect:
        pass
