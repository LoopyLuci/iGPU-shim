use pyo3::prelude::*;

mod inner {
    pub use igpu_ml_core::*;
    pub use igpu_ml_models::*;
}

#[pyfunction]
fn infer_backend(
    model_name: &str,
    shader_complexity: f32,
    vertex_count_log: f32,
    draw_call_rate: f32,
    cache_hit_rate: f32,
    dvfs_headroom: f32,
    thermal_headroom: f32,
    predictor_accuracy: f32,
    is_compute_dispatch: u8,
) -> PyResult<PyObject> {
    let features = inner::FeatureVector {
        shader_complexity_norm: shader_complexity.clamp(0.0, 1.0),
        vertex_count_log_norm: vertex_count_log.clamp(0.0, 1.0),
        draw_call_rate_norm: draw_call_rate.clamp(0.0, 1.0),
        cache_hit_rate: cache_hit_rate.clamp(0.0, 1.0),
        dvfs_headroom: dvfs_headroom.clamp(0.0, 1.0),
        thermal_headroom: thermal_headroom.clamp(0.0, 1.0),
        predictor_accuracy: predictor_accuracy.clamp(0.0, 1.0),
        is_compute_dispatch: if is_compute_dispatch != 0 { 1.0 } else { 0.0 },
    };

    let model: Box<dyn inner::Model> = match model_name {
        "dynamic-tile-transformer" => Box::new(inner::DynamicTileTransformer::new(
            inner::DynamicTileTransformerConfig::default(),
            42,
        )),
        "telemetry-optimizer" => Box::new(inner::TelemetryOptimizer::new(
            inner::TelemetryOptimizerConfig::default(),
            42,
        )),
        other => {
            return Err(pyo3::exceptions::PyValueError::new_err(format!(
                "unknown model: {}",
                other
            )))
        }
    };

    let scores = model.infer(&features);
    Python::with_gil(|gil| {
        let dict = pyo3::types::PyDict::new_bound(gil);
        dict.set_item("scores", scores.scores.to_vec())?;
        dict.set_item("chosen", scores.chosen)?;
        dict.set_item("confidence", scores.confidence)?;
        dict.set_item("epsilon_greedy", scores.epsilon_greedy)?;
        Ok(dict.into())
    })
}

#[pyfunction]
fn observe_backend(
    model_name: &str,
    shader_complexity: f32,
    vertex_count_log: f32,
    draw_call_rate: f32,
    cache_hit_rate: f32,
    dvfs_headroom: f32,
    thermal_headroom: f32,
    predictor_accuracy: f32,
    is_compute_dispatch: u8,
    action: u8,
    reward: f32,
) -> PyResult<PyObject> {
    let features = inner::FeatureVector {
        shader_complexity_norm: shader_complexity.clamp(0.0, 1.0),
        vertex_count_log_norm: vertex_count_log.clamp(0.0, 1.0),
        draw_call_rate_norm: draw_call_rate.clamp(0.0, 1.0),
        cache_hit_rate: cache_hit_rate.clamp(0.0, 1.0),
        dvfs_headroom: dvfs_headroom.clamp(0.0, 1.0),
        thermal_headroom: thermal_headroom.clamp(0.0, 1.0),
        predictor_accuracy: predictor_accuracy.clamp(0.0, 1.0),
        is_compute_dispatch: if is_compute_dispatch != 0 { 1.0 } else { 0.0 },
    };

    let mut model: Box<dyn inner::Model> = match model_name {
        "dynamic-tile-transformer" => Box::new(inner::DynamicTileTransformer::new(
            inner::DynamicTileTransformerConfig::default(),
            42,
        )),
        "telemetry-optimizer" => Box::new(inner::TelemetryOptimizer::new(
            inner::TelemetryOptimizerConfig::default(),
            42,
        )),
        other => {
            return Err(pyo3::exceptions::PyValueError::new_err(format!(
                "unknown model: {}",
                other
            )))
        }
    };

    let experience = inner::Experience {
        features,
        action: action as u32,
        reward,
        timestamp_ns: 0,
        session_id: 0,
        energy_joules: 0.0,
        thermal_events: 0,
        confidence: 0.0,
    };

    let result = model.update(&experience).map_err(|e| {
        pyo3::exceptions::PyRuntimeError::new_err(format!("update failed: {}", e))
    })?;

    Python::with_gil(|gil| {
        let dict = pyo3::types::PyDict::new_bound(gil);
        dict.set_item("loss", result.loss)?;
        dict.set_item("gradient_norm", result.gradient_norm)?;
        dict.set_item("learning_rate", result.learning_rate)?;
        Ok(dict.into())
    })
}

#[pyfunction]
fn list_models() -> Vec<&'static str> {
    vec!["dynamic-tile-transformer", "telemetry-optimizer"]
}

#[pymodule]
fn igpu_ml_python(_py: Python, m: &PyModule) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(infer_backend, m)?)?;
    m.add_function(wrap_pyfunction!(observe_backend, m)?)?;
    m.add_function(wrap_pyfunction!(list_models, m)?)?;
    Ok(())
}
