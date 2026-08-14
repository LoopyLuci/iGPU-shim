use axum::{
    Router,
    http::StatusCode,
    response::{Html, Json},
    routing::{get, post},
};
use igpu_ml_models::{DynamicTileTransformer, DynamicTileTransformerConfig};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use tokio::net::TcpListener;
use tower_http::cors::CorsLayer;

mod inner {
    pub use igpu_ml_core::*;
    // pub use igpu_ml_models::*; // unused
}

#[derive(Clone, Serialize, Deserialize)]
pub struct InferenceResponse {
    pub model: String,
    pub chosen: u32,
    pub scores: [f32; 3],
    pub confidence: f32,
    pub explanation: [f32; 8],
}

#[derive(Clone)]
struct DashboardState {
    models: Arc<Mutex<HashMap<String, Box<dyn inner::Model>>>>,
}

#[tokio::main]
pub async fn run() {
    let state = DashboardState {
        models: Arc::new(Mutex::new(HashMap::new())),
    };

    let app = Router::new()
        .route("/", get(index))
        .route("/api/health", get(health))
        .route("/api/report", get(report))
        .route("/api/ml/infer", post(infer_backend))
        .route("/api/ml/observe", post(observe_outcome))
        .route("/api/ml/explain", post(explain_action))
        .layer(CorsLayer::permissive())
        .with_state(state);

    let listener = TcpListener::bind(("127.0.0.1", 8765))
        .await
        .expect("bind dashboard");
    println!("Dashboard listening on http://127.0.0.1:8765");
    axum::serve(listener, app.into_make_service())
        .await
        .expect("serve dashboard");
}

pub async fn run_with_port(port: u16) {
    let state = DashboardState {
        models: Arc::new(Mutex::new(HashMap::new())),
    };

    let app = Router::new()
        .route("/", get(index))
        .route("/api/health", get(health))
        .route("/api/report", get(report))
        .route("/api/ml/infer", post(infer_backend))
        .route("/api/ml/observe", post(observe_outcome))
        .route("/api/ml/explain", post(explain_action))
        .layer(CorsLayer::permissive())
        .with_state(state);

    let listener = TcpListener::bind(("127.0.0.1", port))
        .await
        .expect("bind dashboard test");
    axum::serve(listener, app.into_make_service())
        .await
        .expect("serve dashboard test");
}

async fn index() -> Html<&'static str> {
    Html(include_str!("../../../../dashboard/templates/index.html"))
}

async fn health() -> Json<serde_json::Value> {
    Json(serde_json::json!({"status":"ok","service":"synapse-dashboard"}))
}

async fn report() -> Json<serde_json::Value> {
    let default_report = serde_json::json!({
        "schema_version": "v2.0.0",
        "heuristic": "temporal_locality_v1",
        "temporal_window_frames": 3,
        "its_predictor": {"total_predictions": 0, "accurate_predictions": 0, "wasted_predictions": 0, "accuracy_rate": 0.0, "waste_rate": 0.0},
        "its_cache": {"hits": 0, "misses": 0, "sync_stalls": 0, "current_usage_bytes": 0, "hit_rate": 0.0},
        "best_horizon": {"window": 5, "stalls_avoided": 0, "false_positives": 0, "energy_efficiency": 0.0},
        "horizon_windows": [],
        "total_frames_analyzed": 0,
        "ml_model": {"total_updates": 0, "cumulative_reward": 0.0, "selection_counts": [0,0,0], "current_epsilon": 0.05, "current_alpha": 0.01, "training_status": "inactive"},
        "dvfs": {"total_transitions": 0, "emergency_overrides": 0, "hysteresis_drops": 0, "total_switch_energy_nj": 0.0},
        "jit": {"cold_cache_fallbacks": 0, "cache_hits": 0, "worst_fallback_ms": 0.0, "total_fallback_ms": 0.0},
        "hai": {"full_draws_emitted": 0, "delta_draws_emitted": 0, "raw_bytes_equivalent": 0, "actual_bytes_emitted": 0, "compression_ratio": 1.0},
        "thermal": {"thermal_mitigation_events": 0, "stability_overrides": 0, "proactive_boosts": 0},
        "power": {"joules_saved": 0.0, "avg_milliwatts_saved_at_60fps": 0.0, "battery_extension_factor": 0.0},
        "backend_routing": {"jit_dispatches": 0, "hai_dispatches": 0, "oracle_dispatches": 0, "total_draw_calls": 0}
    });

    let cwd = std::env::current_dir().unwrap_or_default();
    let mut report_path = cwd.join("report.json");
    if !report_path.exists() {
        report_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../report.json");
    }
    if !report_path.exists() {
        if let Ok(exe) = std::env::current_exe() {
            report_path = exe.parent().unwrap().join("report.json");
        }
    }
    let body = std::fs::read_to_string(&report_path).unwrap_or_default();
    if body.is_empty() {
        Json(default_report)
    } else {
        match serde_json::from_str(&body) {
            Ok(v) => Json(v),
            Err(_) => Json(default_report),
        }
    }
}

async fn infer_backend(
    axum::extract::State(state): axum::extract::State<DashboardState>,
    axum::Json(payload): axum::Json<inner::FeatureVector>,
) -> Result<Json<InferenceResponse>, StatusCode> {
    let mut models = state.models.lock().map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    if !models.contains_key("dynamic-tile-transformer") {
        models.insert(
            "dynamic-tile-transformer".to_string(),
            Box::new(DynamicTileTransformer::new(
                DynamicTileTransformerConfig::default(),
                7,
            )),
        );
    }
    let model = models
        .get_mut("dynamic-tile-transformer")
        .expect("model exists");
    let scores = model.infer(&payload);
    let explanation = model.explain(&payload);
    Ok(Json(InferenceResponse {
        model: "dynamic-tile-transformer".into(),
        chosen: scores.chosen,
        scores: scores.scores,
        confidence: scores.confidence,
        explanation: explanation.feature_importances,
    }))
}

async fn observe_outcome(
    axum::extract::State(state): axum::extract::State<DashboardState>,
    axum::Json(payload): axum::Json<serde_json::Value>,
) -> Result<Json<serde_json::Value>, StatusCode> {
    let features = match payload
        .get("features")
        .and_then(|v| serde_json::from_value(v.clone()).ok())
    {
        Some(v) => v,
        None => return Err(StatusCode::BAD_REQUEST),
    };
    let action = payload.get("action").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
    let reward = payload.get("reward").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32;
    let mut models = state.models.lock().map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    if !models.contains_key("dynamic-tile-transformer") {
        models.insert(
            "dynamic-tile-transformer".to_string(),
            Box::new(DynamicTileTransformer::new(
                DynamicTileTransformerConfig::default(),
                7,
            )),
        );
    }
    let model = models
        .get_mut("dynamic-tile-transformer")
        .expect("model exists");
    let experience = inner::Experience {
        features,
        action,
        reward,
        timestamp_ns: std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos() as u64,
        session_id: 0,
        energy_joules: 0.0,
        thermal_events: 0,
        confidence: 0.0,
    };
    let result = model.update(&experience).expect("update");
    Ok(Json(serde_json::json!({
        "loss": result.loss,
        "gradient_norm": result.gradient_norm,
        "learning_rate": result.learning_rate
    })))
}

async fn explain_action(
    axum::extract::State(state): axum::extract::State<DashboardState>,
    axum::Json(payload): axum::Json<inner::FeatureVector>,
) -> Result<Json<[f32; 8]>, StatusCode> {
    let mut models = state.models.lock().map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    if !models.contains_key("dynamic-tile-transformer") {
        models.insert(
            "dynamic-tile-transformer".to_string(),
            Box::new(DynamicTileTransformer::new(
                DynamicTileTransformerConfig::default(),
                7,
            )),
        );
    }
    let model = models
        .get_mut("dynamic-tile-transformer")
        .expect("model exists");
    let explanation = model.explain(&payload);
    Ok(Json(explanation.feature_importances))
}
