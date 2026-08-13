use std::time::{SystemTime, UNIX_EPOCH};

use crate::{BatchResult, Experience, IgpuMlError, Model};

#[derive(Clone, Debug)]
pub struct TrainerConfig {
    pub batch_size: usize,
    pub learning_rate: f32,
    pub l2_lambda: f32,
    pub max_gradient_norm: f32,
    pub energy_weight: f32,
    pub performance_weight: f32,
}

impl Default for TrainerConfig {
    fn default() -> Self {
        Self {
            batch_size: 32,
            learning_rate: 0.01,
            l2_lambda: 0.001,
            max_gradient_norm: 1.0,
            energy_weight: 0.7,
            performance_weight: 0.3,
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Trainer {
    config: TrainerConfig,
    step: u64,
}

impl Trainer {
    pub fn new(config: TrainerConfig) -> Self {
        Self { config, step: 0 }
    }

    pub fn train_batch<M: Model>(
        &mut self,
        model: &mut M,
        batch: &[Experience],
    ) -> std::result::Result<BatchResult, IgpuMlError> {
        if batch.is_empty() {
            return Ok(BatchResult::default());
        }

        let t0 = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_micros() as u64)
            .unwrap_or(0);

        let mut total_loss = 0.0f32;
        for exp in batch {
            let result = model.update(exp)?;
            total_loss += result.loss;
        }

        let t1 = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_micros() as u64)
            .unwrap_or(0);

        self.step += 1;
        Ok(BatchResult {
            avg_loss: total_loss / batch.len() as f32,
            samples: batch.len(),
            duration_us: t1.saturating_sub(t0),
        })
    }

    pub fn energy_aware_loss(
        &self,
        reward: f32,
        energy_joules: f32,
        thermal_events: u32,
    ) -> f32 {
        let energy_term = self.config.energy_weight * (1.0 / (1.0 + energy_joules.max(0.0)));
        let perf_term = self.config.performance_weight * reward;
        let thermal_term = (thermal_events as f32) * 0.1;
        -(energy_term + perf_term - thermal_term)
    }

    pub fn step(&self) -> u64 {
        self.step
    }

    pub fn learning_rate(&self) -> f32 {
        self.config.learning_rate
    }
}
