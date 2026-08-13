use serde::{Deserialize, Serialize};

#[repr(C)]
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Experience {
    pub features: crate::FeatureVector,
    pub action: u32,
    pub reward: f32,
    pub timestamp_ns: u64,
    pub session_id: u64,
    pub energy_joules: f32,
    pub thermal_events: u32,
    pub confidence: f32,
}

impl Experience {
    pub fn new(features: crate::FeatureVector, action: u32, reward: f32) -> Self {
        let timestamp_ns = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0);
        Self {
            features,
            action,
            reward,
            timestamp_ns,
            session_id: 0,
            energy_joules: 0.0,
            thermal_events: 0,
            confidence: 0.0,
        }
    }
}
