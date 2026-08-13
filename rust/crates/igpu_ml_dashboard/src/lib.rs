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
    pub use igpu_ml_models::*;
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
async fn main() {
    let state = DashboardState {
        models: Arc::new(Mutex::new(HashMap::new())),
    };

    let app = Router::new()
        .route("/", get(index))
        .route("/api/health", get(health))
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

async fn index() -> Html<&'static str> {
    Html(include_str!("../../../../dashboard/templates/index.html"))
}

async fn health() -> Json<serde_json::Value> {
    Json(serde_json::json!({"status":"ok","service":"synapse-dashboard"}))
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
