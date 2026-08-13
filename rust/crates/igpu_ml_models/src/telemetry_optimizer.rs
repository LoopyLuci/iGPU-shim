use igpu_ml_core::{
    Experience, FeatureVector, IgpuMlError, Model, UpdateResult,
};
use ndarray::{Array, Array1, Array2};
use rand::{Rng, SeedableRng};
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct TelemetryOptimizerConfig {
    pub input_dim: usize,
    pub hidden_dims: Vec<usize>,
    pub output_dim: usize,
    pub learning_rate: f32,
    pub epsilon: f32,
    pub l2_lambda: f32,
    pub energy_weight: f32,
    pub performance_weight: f32,
    pub thermal_penalty: f32,
}

impl Default for TelemetryOptimizerConfig {
    fn default() -> Self {
        Self {
            input_dim: 8,
            hidden_dims: vec![24, 12],
            output_dim: 3,
            learning_rate: 0.005,
            epsilon: 0.05,
            l2_lambda: 0.0005,
            energy_weight: 0.7,
            performance_weight: 0.3,
            thermal_penalty: 0.1,
        }
    }
}

pub struct TelemetryOptimizer {
    config: TelemetryOptimizerConfig,
    weights: Vec<Array2<f32>>,
    biases: Vec<Array1<f32>>,
    rng: rand::rngs::StdRng,
    version: String,
    training_steps: u64,
}

impl TelemetryOptimizer {
    pub fn new(config: TelemetryOptimizerConfig, seed: u64) -> Self {
        let mut rng = rand::rngs::StdRng::seed_from_u64(seed);
        let mut weights = Vec::new();
        let mut biases = Vec::new();

        let mut prev_dim = config.input_dim;
        for &hidden_dim in &config.hidden_dims {
            let scale = (2.0 / (prev_dim + hidden_dim) as f32).sqrt();
            let w = Array::from_shape_fn((hidden_dim, prev_dim), |(_, _)| {
                (rng.gen::<f32>() - 0.5) * 2.0 * scale
            });
            let b = Array::zeros(hidden_dim);
            weights.push(w);
            biases.push(b);
            prev_dim = hidden_dim;
        }

        let scale = (2.0 / (prev_dim + config.output_dim) as f32).sqrt();
        let w_out = Array::from_shape_fn((config.output_dim, prev_dim), |(_, _)| {
            (rng.gen::<f32>() - 0.5) * 2.0 * scale
        });
        let b_out = Array::zeros(config.output_dim);
        weights.push(w_out);
        biases.push(b_out);

        Self {
            config,
            weights,
            biases,
            rng,
            version: "1.0.0".to_string(),
            training_steps: 0,
        }
    }

    fn forward(&self, features: &FeatureVector) -> Array1<f32> {
        let mut x = features.to_array();
        for (w, b) in self.weights.iter().zip(self.biases.iter()).take(self.weights.len() - 1) {
            x = w.dot(&x) + b;
            x.mapv_inplace(|v| v.max(0.0));
        }
        let (w_out, b_out) = self.weights.last().zip(self.biases.last()).unwrap();
        w_out.dot(&x) + b_out
    }
}

impl Model for TelemetryOptimizer {
    fn id(&self) -> &str { "telemetry-optimizer" }
    fn version(&self) -> &str { &self.version }

    fn infer(&self, features: &FeatureVector) -> igpu_ml_core::ActionScores {
        if let Err(_) = features.validate() {
            return igpu_ml_core::ActionScores {
                scores: [0.333, 0.333, 0.334],
                chosen: igpu_ml_core::Backend::Oracle as u32,
                confidence: 0.0,
                epsilon_greedy: false,
            };
        }

        let logits = self.forward(features);
        let max_logit = logits.iter().copied().reduce(f32::max).unwrap_or(0.0);
        let exp: Array1<f32> = logits.mapv(|v| (v - max_logit).exp());
        let sum: f32 = exp.sum();
        let probs = if sum > 0.0 { exp / sum } else { Array1::zeros(3) };

        let mut scores = [0.0f32; 3];
        scores.copy_from_slice(probs.as_slice().unwrap_or(&[0.0; 3]));

        let confidence = scores.iter().copied().reduce(f32::max).unwrap_or(0.0)
            - scores.iter().copied().sum::<f32>() / 3.0;

        igpu_ml_core::ActionScores {
            scores,
            chosen: scores.iter().copied().enumerate().max_by(|a, b| a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal)).map(|(i, _)| i as u32).unwrap_or(2),
            confidence: confidence.clamp(0.0, 1.0),
            epsilon_greedy: false,
        }
    }

    fn update(&mut self, experience: &Experience) -> Result<UpdateResult, IgpuMlError> {
        let features = &experience.features;
        if let Err(_) = features.validate() { return Ok(UpdateResult::default()); }

        let action_idx = experience.action as usize;
        if action_idx >= 3 { return Ok(UpdateResult::default()); }

        let mut current = features.to_array();
        let mut activations = vec![current.clone()];

        for (w, b) in self.weights.iter().zip(self.biases.iter()).take(self.weights.len() - 1) {
            current = w.dot(&current) + b;
            current.mapv_inplace(|v| v.max(0.0));
            activations.push(current.clone());
        }

        let (w_out, b_out) = self.weights.last().zip(self.biases.last()).unwrap();
        let logits = w_out.dot(&current) + b_out;

        let max_logit = logits.iter().copied().reduce(f32::max).unwrap_or(0.0);
        let exp: Array1<f32> = logits.mapv(|v| (v - max_logit).exp());
        let sum: f32 = exp.sum();
        let mut probs = if sum > 0.0 { exp / sum } else { Array1::zeros(3) };
        probs[action_idx] -= 1.0;

        let lr = self.config.learning_rate;
        let l2 = self.config.l2_lambda;
        let mut delta = probs;
        let mut total_norm: f32 = 0.0;

        // Output layer
        let out_idx = self.weights.len() - 1;
        let input = activations.get(out_idx).cloned().unwrap_or_default();
        {
            let w = &mut self.weights[out_idx];
            let b = &mut self.biases[out_idx];
            for j in 0..w.nrows() {
                for k in 0..w.ncols() {
                    let grad = delta[j] * input[k] + l2 * w[(j, k)];
                    w[(j, k)] -= lr * grad;
                    total_norm += grad * grad;
                }
                b[j] -= lr * delta[j];
                total_norm += delta[j] * delta[j];
            }
        }

        // Hidden layers
        for i in (0..self.weights.len() - 1).rev() {
            let w_next = &self.weights[i + 1];
            let mut new_delta = Array1::zeros(w_next.ncols());
            for j in 0..w_next.ncols() {
                for k in 0..delta.len() {
                    new_delta[j] += w_next[(k, j)] * delta[k];
                }
            }
            let act = activations.get(i + 1).cloned().unwrap_or_default();
            for j in 0..new_delta.len() {
                new_delta[j] *= if act[j] > 0.0 { 1.0 } else { 0.0 };
            }
            delta = new_delta;

            let input = activations.get(i).cloned().unwrap_or_default();
            let w = &mut self.weights[i];
            let b = &mut self.biases[i];
            for j in 0..w.nrows() {
                for k in 0..w.ncols() {
                    let grad = delta[j] * input[k] + l2 * w[(j, k)];
                    w[(j, k)] -= lr * grad;
                    total_norm += grad * grad;
                }
                b[j] -= lr * delta[j];
                total_norm += delta[j] * delta[j];
            }
        }

        let energy_term = self.config.energy_weight * experience.energy_joules;
        let thermal_term = self.config.thermal_penalty * experience.thermal_events as f32;
        let perf_term = self.config.performance_weight * experience.reward;
        let loss = energy_term + thermal_term - perf_term;

        self.training_steps += 1;

        Ok(UpdateResult {
            loss,
            gradient_norm: total_norm.sqrt(),
            learning_rate: lr,
        })
    }

    fn batch_update(&mut self, batch: &[Experience]) -> Result<igpu_ml_core::BatchResult, IgpuMlError> {
        let t0 = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_micros() as u64).unwrap_or(0);
        let mut total_loss = 0.0f32;
        for exp in batch { total_loss += self.update(exp)?.loss; }
        let t1 = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_micros() as u64).unwrap_or(0);

        Ok(igpu_ml_core::BatchResult {
            avg_loss: if batch.is_empty() { 0.0 } else { total_loss / batch.len() as f32 },
            samples: batch.len(),
            duration_us: t1.saturating_sub(t0),
        })
    }

    fn serialize(&self) -> Vec<u8> {
        let mut hasher = blake3::Hasher::new();
        hasher.update(b"telemetry-optimizer-v1");

        let config_bytes = bincode::serialize(&self.config).unwrap_or_default();
        hasher.update(&config_bytes);

        let mut weight_bytes = Vec::new();
        for (w, b) in self.weights.iter().zip(self.biases.iter()) {
            let wb = bincode::serialize(w).unwrap_or_default();
            let bb = bincode::serialize(b).unwrap_or_default();
            hasher.update(&wb);
            hasher.update(&bb);
            weight_bytes.extend_from_slice(&(wb.len() as u32).to_le_bytes());
            weight_bytes.extend_from_slice(&wb);
            weight_bytes.extend_from_slice(&(bb.len() as u32).to_le_bytes());
            weight_bytes.extend_from_slice(&bb);
        }

        let checksum = *hasher.finalize().as_bytes();
        let mut out = Vec::new();
        let version_bytes = self.version.as_bytes();
        out.extend_from_slice(&(version_bytes.len() as u16).to_le_bytes());
        out.extend_from_slice(version_bytes);
        out.extend_from_slice(&checksum);
        out.extend_from_slice(&(config_bytes.len() as u32).to_le_bytes());
        out.extend_from_slice(&config_bytes);
        out.extend_from_slice(&weight_bytes);
        out
    }

    fn deserialize(&mut self, bytes: &[u8]) -> Result<(), IgpuMlError> {
        let mut cursor = 0usize;
        let version_len = u16::from_le_bytes([bytes[cursor], bytes[cursor + 1]]) as usize;
        cursor += 2;
        self.version = std::str::from_utf8(&bytes[cursor..cursor + version_len]).map_err(|e| IgpuMlError::DeserializationError(e.to_string()))?.to_string();
        cursor += version_len;
        let checksum = &bytes[cursor..cursor + 32];
        cursor += 32;

        let config_len = u32::from_le_bytes([bytes[cursor], bytes[cursor + 1], bytes[cursor + 2], bytes[cursor + 3]]) as usize;
        cursor += 4;
        let config_bytes = &bytes[cursor..cursor + config_len];
        cursor += config_len;
        self.config = bincode::deserialize(config_bytes).map_err(|e| IgpuMlError::DeserializationError(e.to_string()))?;

        self.weights.clear();
        self.biases.clear();
        let mut hasher = blake3::Hasher::new();
        hasher.update(b"telemetry-optimizer-v1");
        hasher.update(config_bytes);

        for _ in 0..(self.config.hidden_dims.len() + 1) {
            let w_len = u32::from_le_bytes([bytes[cursor], bytes[cursor + 1], bytes[cursor + 2], bytes[cursor + 3]]) as usize;
            cursor += 4;
            let w: Array2<f32> = bincode::deserialize(&bytes[cursor..cursor + w_len]).map_err(|e| IgpuMlError::DeserializationError(e.to_string()))?;
            cursor += w_len;
            self.weights.push(w);

            let b_len = u32::from_le_bytes([bytes[cursor], bytes[cursor + 1], bytes[cursor + 2], bytes[cursor + 3]]) as usize;
            cursor += 4;
            let b: Array1<f32> = bincode::deserialize(&bytes[cursor..cursor + b_len]).map_err(|e| IgpuMlError::DeserializationError(e.to_string()))?;
            cursor += b_len;
            self.biases.push(b);

            let wb = bincode::serialize(self.weights.last().unwrap()).unwrap_or_default();
            let bb = bincode::serialize(self.biases.last().unwrap()).unwrap_or_default();
            hasher.update(&wb);
            hasher.update(&bb);
        }

        let computed = *hasher.finalize().as_bytes();
        if computed != checksum {
            return Err(IgpuMlError::DeserializationError("checksum mismatch".to_string()));
        }
        Ok(())
    }

    fn explain(&self, features: &FeatureVector) -> igpu_ml_core::ModelExplanation {
        let predicted_scores = self.infer(features).scores;
        let confidence = predicted_scores.iter().copied().reduce(f32::max).unwrap_or(0.0)
            - predicted_scores.iter().copied().sum::<f32>() / 3.0;
        igpu_ml_core::ModelExplanation {
            feature_importances: [
                features.shader_complexity_norm,
                features.vertex_count_log_norm,
                features.draw_call_rate_norm,
                features.cache_hit_rate,
                features.dvfs_headroom,
                features.thermal_headroom,
                features.predictor_accuracy,
                features.is_compute_dispatch,
            ],
            predicted_scores,
            confidence: confidence.clamp(0.0, 1.0),
            model_version: self.version.clone(),
            notes: format!("training_steps={}", self.training_steps),
        }
    }
}
