use crate::error::{IgpuMlError, Result};
use serde::{Deserialize, Serialize};

#[repr(C)]
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Experience {
    pub features: crate::FeatureVector,
    pub action: u32, // Backend as u32
    pub reward: f32,
    pub timestamp_ns: u64,
    pub session_id: u64,
}

impl Experience {
    pub fn new(features: crate::FeatureVector, action: crate::Backend, reward: f32) -> Self {
        Self {
            features,
            action: action.to_u8() as u32,
            reward,
            timestamp_ns: std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_nanos() as u64)
                .unwrap_or(0),
            session_id: 0,
        }
    }

    pub fn action_backend(&self) -> Result<crate::Backend> {
        crate::Backend::from_u8(self.action as u8)
            .ok_or_else(|| IgpuMlError::InvalidAction(self.action))
    }
}
