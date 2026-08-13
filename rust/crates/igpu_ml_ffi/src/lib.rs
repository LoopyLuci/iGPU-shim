use igpu_ml_core::{
    ActionScores, Backend, Experience, FeatureVector, ModelExplanation, UpdateResult,
};
use igpu_ml_models::{DynamicTileTransformer, DynamicTileTransformerConfig, TelemetryOptimizer, TelemetryOptimizerConfig};
use std::collections::HashMap;
use std::ffi::c_char;
use std::os::raw::c_void;
use std::sync::{LazyLock, Mutex};

mod inner {
    pub use igpu_ml_core::*;
    
}

#[repr(C)]
pub struct SynapseFeatureVector {
    pub shader_complexity_norm: f32,
    pub vertex_count_log_norm: f32,
    pub draw_call_rate_norm: f32,
    pub cache_hit_rate: f32,
    pub dvfs_headroom: f32,
    pub thermal_headroom: f32,
    pub predictor_accuracy: f32,
    pub is_compute_dispatch: f32,
}

#[repr(C)]
pub struct SynapseExperience {
    pub features: SynapseFeatureVector,
    pub action: u32,
    pub reward: f32,
    pub timestamp_ns: u64,
    pub session_id: u64,
    pub energy_joules: f32,
    pub thermal_events: u32,
    pub confidence: f32,
}

#[repr(C)]
pub struct SynapseActionScores {
    pub scores: [f32; 3],
    pub chosen: u32,
    pub confidence: f32,
    pub epsilon_greedy: bool,
}

#[repr(C)]
pub struct SynapseModelExplanation {
    pub feature_importances: [f32; 8],
    pub predicted_scores: [f32; 3],
    pub confidence: f32,
    pub model_version: *const c_char,
    pub notes: *const c_char,
}

#[repr(C)]
pub struct SynapseUpdateResult {
    pub loss: f32,
    pub gradient_norm: f32,
    pub learning_rate: f32,
}

#[repr(C)]
pub struct SynapseBatchResult {
    pub avg_loss: f32,
    pub samples: usize,
    pub duration_us: u64,
}

#[derive(Default)]
struct ModelStore {
    models: Mutex<HashMap<u64, Box<dyn inner::Model>>>,
    next_id: std::sync::atomic::AtomicU64,
}

impl ModelStore {
    fn insert(&self, model: Box<dyn inner::Model>) -> u64 {
        let id = self
            .next_id
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let mut guard = self.models.lock().unwrap();
        guard.insert(id, model);
        id
    }

    fn get_mut(&self, id: u64) -> Option<std::sync::MutexGuard<'_, HashMap<u64, Box<dyn inner::Model>>>> {
        let guard = self.models.lock().unwrap();
        if guard.contains_key(&id) {
            Some(guard)
        } else {
            None
        }
    }

    fn remove(&self, id: u64) -> Option<Box<dyn inner::Model>> {
        let mut guard = self.models.lock().unwrap();
        guard.remove(&id)
    }
}

static GLOBAL_STORE: LazyLock<ModelStore> = LazyLock::new(ModelStore::default);

#[no_mangle]
pub unsafe extern "C" fn synapse_ml_create_model(model_name: *const c_char) -> u64 {
    if model_name.is_null() {
        return u64::MAX;
    }
    let name = match std::ffi::CStr::from_ptr(model_name).to_str() {
        Ok(s) => s,
        Err(_) => return u64::MAX,
    };

    let model: Box<dyn inner::Model> = match name {
        "dynamic-tile-transformer" => Box::new(DynamicTileTransformer::new(
            DynamicTileTransformerConfig::default(),
            42,
        )),
        "telemetry-optimizer" => Box::new(TelemetryOptimizer::new(
            TelemetryOptimizerConfig::default(),
            42,
        )),
        _ => return u64::MAX,
    };

    GLOBAL_STORE.insert(model)
}

#[no_mangle]
pub unsafe extern "C" fn synapse_ml_destroy_model(handle: u64) -> i32 {
    if handle == u64::MAX {
        return -1;
    }
    GLOBAL_STORE.remove(handle);
    0
}

#[no_mangle]
pub unsafe extern "C" fn synapse_ml_infer(
    handle: u64,
    features: *const SynapseFeatureVector,
) -> SynapseActionScores {
    if features.is_null() {
        return fallback_scores();
    }
    let fv = feature_vector_from_raw(&*features);
    let result = GLOBAL_STORE.get_mut(handle).and_then(|mut guard| {
        guard.get_mut(&handle).map(|m| m.infer(&fv))
    });
    match result {
        Some(scores) => action_scores_to_raw(scores),
        None => fallback_scores(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn synapse_ml_observe(
    handle: u64,
    experience: *const SynapseExperience,
) -> SynapseUpdateResult {
    if experience.is_null() {
        return SynapseUpdateResult {
            loss: 0.0,
            gradient_norm: 0.0,
            learning_rate: 0.0,
        };
    }
    let exp = experience_from_raw(&*experience);
    let result = GLOBAL_STORE.get_mut(handle).and_then(|mut guard| {
        guard.get_mut(&handle).and_then(|m| m.update(&exp).ok())
    });
    match result {
        Some(r) => update_result_to_raw(r),
        None => SynapseUpdateResult {
            loss: 0.0,
            gradient_norm: 0.0,
            learning_rate: 0.0,
        },
    }
}

#[no_mangle]
pub unsafe extern "C" fn synapse_ml_explain(
    handle: u64,
    features: *const SynapseFeatureVector,
) -> SynapseModelExplanation {
    if features.is_null() {
        return empty_explanation();
    }
    let fv = feature_vector_from_raw(&*features);
    let result = GLOBAL_STORE.get_mut(handle).and_then(|mut guard| {
        guard.get_mut(&handle).map(|m| m.explain(&fv))
    });
    match result {
        Some(ex) => explanation_to_raw(ex),
        None => empty_explanation(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn synapse_ml_last_error() -> *const c_char {
    static LAST_ERROR: std::sync::atomic::AtomicPtr<c_char> =
        std::sync::atomic::AtomicPtr::new(std::ptr::null_mut());
    LAST_ERROR.load(std::sync::atomic::Ordering::Relaxed)
}

#[no_mangle]
pub unsafe extern "C" fn synapse_ml_free_string(s: *mut c_char) {
    if !s.is_null() {
        libc::free(s as *mut c_void);
    }
}

fn feature_vector_from_raw(f: &SynapseFeatureVector) -> FeatureVector {
    FeatureVector {
        shader_complexity_norm: f.shader_complexity_norm.clamp(0.0, 1.0),
        vertex_count_log_norm: f.vertex_count_log_norm.clamp(0.0, 1.0),
        draw_call_rate_norm: f.draw_call_rate_norm.clamp(0.0, 1.0),
        cache_hit_rate: f.cache_hit_rate.clamp(0.0, 1.0),
        dvfs_headroom: f.dvfs_headroom.clamp(0.0, 1.0),
        thermal_headroom: f.thermal_headroom.clamp(0.0, 1.0),
        predictor_accuracy: f.predictor_accuracy.clamp(0.0, 1.0),
        is_compute_dispatch: f.is_compute_dispatch.clamp(0.0, 1.0),
    }
}

fn experience_from_raw(e: &SynapseExperience) -> Experience {
    Experience {
        features: feature_vector_from_raw(&e.features),
        action: e.action,
        reward: e.reward,
        timestamp_ns: e.timestamp_ns,
        session_id: e.session_id,
        energy_joules: e.energy_joules,
        thermal_events: e.thermal_events,
        confidence: e.confidence,
    }
}

fn action_scores_to_raw(scores: ActionScores) -> SynapseActionScores {
    SynapseActionScores {
        scores: scores.scores,
        chosen: scores.chosen,
        confidence: scores.confidence,
        epsilon_greedy: scores.epsilon_greedy,
    }
}

fn update_result_to_raw(r: UpdateResult) -> SynapseUpdateResult {
    SynapseUpdateResult {
        loss: r.loss,
        gradient_norm: r.gradient_norm,
        learning_rate: r.learning_rate,
    }
}

fn explanation_to_raw(ex: ModelExplanation) -> SynapseModelExplanation {
    let version = std::ffi::CString::new(ex.model_version).unwrap_or_default();
    let notes = std::ffi::CString::new(ex.notes).unwrap_or_default();
    SynapseModelExplanation {
        feature_importances: ex.feature_importances,
        predicted_scores: ex.predicted_scores,
        confidence: ex.confidence,
        model_version: version.into_raw(),
        notes: notes.into_raw(),
    }
}

fn empty_explanation() -> SynapseModelExplanation {
    SynapseModelExplanation {
        feature_importances: [0.0; 8],
        predicted_scores: [0.0; 3],
        confidence: 0.0,
        model_version: std::ptr::null(),
        notes: std::ptr::null(),
    }
}

fn fallback_scores() -> SynapseActionScores {
    SynapseActionScores {
        scores: [0.333, 0.333, 0.334],
        chosen: Backend::Oracle as u32,
        confidence: 0.0,
        epsilon_greedy: false,
    }
}
