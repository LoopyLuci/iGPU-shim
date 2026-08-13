use igpu_ml_core::Experience;
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ContinualLearningConfig {
    pub replay_capacity: usize,
    pub replay_sample_ratio: f32,
    pub energy_weight: f32,
    pub performance_weight: f32,
    pub thermal_penalty: f32,
    pub l2_lambda: f32,
    pub max_gradient_norm: f32,
    pub learning_rate: f32,
}

impl Default for ContinualLearningConfig {
    fn default() -> Self {
        Self {
            replay_capacity: 4096,
            replay_sample_ratio: 0.3,
            energy_weight: 0.7,
            performance_weight: 0.3,
            thermal_penalty: 0.1,
            l2_lambda: 0.001,
            max_gradient_norm: 1.0,
            learning_rate: 0.01,
        }
    }
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct TelemetryStats {
    pub total_energy_joules: f32,
    pub total_thermal_events: u32,
    pub total_frames: u64,
    pub avg_cache_hit: f32,
    pub avg_dvfs_headroom: f32,
    pub backend_counts: [u64; 3],
    pub confidence_sum: f32,
    pub confidence_count: u64,
}

impl TelemetryStats {
    pub fn observe(&mut self, experience: &Experience) {
        self.total_frames += 1;
        self.total_energy_joules += experience.energy_joules;
        self.total_thermal_events += experience.thermal_events;
        self.avg_cache_hit = self.avg_cache_hit * 0.95 + experience.features.cache_hit_rate * 0.05;
        self.avg_dvfs_headroom = self.avg_dvfs_headroom * 0.95 + experience.features.dvfs_headroom * 0.05;
        self.confidence_sum += experience.confidence;
        self.confidence_count += 1;
        let backend_idx = experience.action as usize;
        if backend_idx < 3 {
            self.backend_counts[backend_idx] += 1;
        }
    }

    pub fn avg_confidence(&self) -> f32 {
        if self.confidence_count == 0 {
            0.0
        } else {
            self.confidence_sum / self.confidence_count as f32
        }
    }

    pub fn energy_per_frame(&self) -> f32 {
        if self.total_frames == 0 {
            0.0
        } else {
            self.total_energy_joules / self.total_frames as f32
        }
    }

    pub fn thermal_rate(&self) -> f32 {
        if self.total_frames == 0 {
            0.0
        } else {
            self.total_thermal_events as f32 / self.total_frames as f32
        }
    }
}

pub struct ContinualLearningEngine {
    config: ContinualLearningConfig,
    trainer: igpu_ml_core::Trainer,
    stats: TelemetryStats,
    step: u64,
}

impl ContinualLearningEngine {
    pub fn new(config: ContinualLearningConfig) -> Self {
        let trainer = igpu_ml_core::Trainer::new(igpu_ml_core::TrainerConfig {
            batch_size: (config.replay_sample_ratio * 32.0) as usize,
            learning_rate: config.learning_rate,
            l2_lambda: config.l2_lambda,
            max_gradient_norm: config.max_gradient_norm,
            energy_weight: config.energy_weight,
            performance_weight: config.performance_weight,
        });
        Self {
            config,
            trainer,
            stats: TelemetryStats::default(),
            step: 0,
        }
    }

    pub fn observe(&mut self, experience: Experience) {
        self.stats.observe(&experience);
    }

    pub fn stats(&self) -> &TelemetryStats {
        &self.stats
    }

    pub fn step(&self) -> u64 {
        self.step
    }

    pub fn energy_aware_loss(&self, reward: f32, energy_joules: f32, thermal_events: u32) -> f32 {
        self.trainer.energy_aware_loss(reward, energy_joules, thermal_events)
    }

    pub fn config(&self) -> &ContinualLearningConfig {
        &self.config
    }
}
