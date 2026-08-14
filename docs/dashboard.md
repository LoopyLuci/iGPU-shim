# iGPU Dashboard

## Launch
```bat
scripts\run_dashboard.bat
```
The Rust Axum server starts on `http://127.0.0.1:8765`.

## Endpoints
| Path | Method | Description |
| --- | --- | --- |
| `/` | GET | Dashboard UI |
| `/api/health` | GET | Service health |
| `/api/report` | GET | C++ session `report.json` |
| `/api/ml/infer` | POST | ML inference |
| `/api/ml/observe` | POST | Outcome observation |
| `/api/ml/explain` | POST | Model explanation |

## `report.json`
- C++ sessions write to `report.json` on shutdown
- Dashboard serves live data when present
- Falls back to defaults when absent

## Smoke tests
```bat
python scripts/test_report_e2e.py
scripts\test_dashboard_smoke.bat
```

## Troubleshooting
- Port in use: another dashboard instance is running; stop it or wait for the port to free
- Missing report: start a C++ session with report writing enabled
