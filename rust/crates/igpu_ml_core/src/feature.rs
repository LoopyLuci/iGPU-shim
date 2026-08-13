use super::error::{IgpuMlError, Result};
use serde::{Deserialize, Serialize};

/// 8-dimensional feature vector matching the C++ FeatureEncoder output.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct FeatureVector {
    pub shader_complexity_norm: f32,
    pub vertex_count_log_norm: f32,
    pub draw_call_rate_norm: f32,
    pub cache_hit_rate: f32,
    pub dvfs_headroom: f32,
    pub thermal_headroom: f32,
    pub predictor_accuracy: f32,
    pub is_compute_dispatch: f32,
}

impl FeatureVector {
    pub const DIM: usize = 8;

    /// Validate that all values are finite and within [0, 1].
    pub fn validate(&self) -> Result<()> {
        let vals = [
            self.shader_complexity_norm,
            self.vertex_count_log_norm,
            self.draw_call_rate_norm,
            self.cache_hit_rate,
            self.dvfs_headroom,
            self.thermal_headroom,
            self.predictor_accuracy,
            self.is_compute_dispatch,
        ];
        for (i, &v) in vals.iter().enumerate() {
            if !v.is_finite() || v < 0.0 || v > 1.0 {
                return Err(IgpuMlError::InvalidFeatureVector(format!(
                    "feature[{}] = {} out of range [0,1]",
                    i, v
                )));
            }
        }
        Ok(())
    }

    /// Convert to a flat `ndarray` for model consumption.
    pub fn to_array(&self) -> ndarray::Array1<f32> {
        ndarray::array![
            self.shader_complexity_norm,
            self.vertex_count_log_norm,
            self.draw_call_rate_norm,
            self.cache_hit_rate,
            self.dvfs_headroom,
            self.thermal_headroom,
            self.predictor_accuracy,
            self.is_compute_dispatch,
        ]
    }

    /// Convert from a flat array, validating bounds.
    pub fn from_array(arr: ndarray::ArrayView1<f32>) -> Result<Self> {
        if arr.len() != Self::DIM {
            return Err(IgpuMlError::InvalidFeatureVector(format!(
                "expected {} dims, got {}",
                Self::DIM,
                arr.len()
            )));
        }
        let fv = Self {
            shader_complexity_norm: arr[0],
            vertex_count_log_norm: arr[1],
            draw_call_rate_norm: arr[2],
            cache_hit_rate: arr[3],
            dvfs_headroom: arr[4],
            thermal_headroom: arr[5],
            predictor_accuracy: arr[6],
            is_compute_dispatch: arr[7],
        };
        fv.validate()?;
        Ok(fv)
    }
}
