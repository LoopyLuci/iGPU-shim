use igpu_ml_core::{Experience, FeatureVector};
use igpu_ml_models::{DynamicTileTransformer, DynamicTileTransformerConfig, TelemetryOptimizer, TelemetryOptimizerConfig};

#[test]
fn dtt_forward_returns_three_scores() {
    let model = DynamicTileTransformer::new(DynamicTileTransformerConfig::default(), 7);
    let fv = FeatureVector {
        shader_complexity_norm: 0.1,
        vertex_count_log_norm: 0.2,
        draw_call_rate_norm: 0.3,
        cache_hit_rate: 0.4,
        dvfs_headroom: 0.5,
        thermal_headroom: 0.6,
        predictor_accuracy: 0.7,
        is_compute_dispatch: 0.0,
    };
    let scores = model.infer(&fv);
    assert_eq!(scores.scores.len(), 3);
    assert!((0.0..=1.0).contains(&scores.confidence));
}

#[test]
fn dtt_update_changes_state() {
    let mut model = DynamicTileTransformer::new(DynamicTileTransformerConfig::default(), 9);
    let fv = FeatureVector::default();
    let first = model.infer(&fv).scores.clone();
    let experience = Experience {
        features: fv,
        action: 0,
        reward: 0.9,
        timestamp_ns: 0,
        session_id: 0,
        energy_joules: 0.0,
        thermal_events: 0,
        confidence: 0.0,
    };
    let _ = model.update(&experience);
    let second = model.infer(&fv).scores.clone();
    assert_ne!(first, second);
}

#[test]
fn telemetry_optimizer_defaults() {
    let model = TelemetryOptimizer::new(TelemetryOptimizerConfig::default(), 13);
    let fv = FeatureVector::default();
    let scores = model.infer(&fv);
    assert_eq!(scores.scores.len(), 3);
    assert!((0.0..=1.0).contains(&scores.confidence));
}

#[test]
fn feature_vector_validation_rejects_nan() {
    let mut fv = FeatureVector::default();
    fv.shader_complexity_norm = f32::NAN;
    assert!(fv.validate().is_err());
}
