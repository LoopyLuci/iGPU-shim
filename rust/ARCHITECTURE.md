# Synapse ML — Architecture Design Document

## 1. Vision

Build a **next-generation, production-grade, polyglot ML system** purpose-built for integrated GPUs (iGPUs) that:
- Makes iGPUs as close to discrete GPU performance as possible
- Learns continuously from workload telemetry
- Optimizes for **energy efficiency**, **thermal constraints**, and **shared-memory bandwidth**
- Is **modular**, **scalable**, and **evolvable** for the next 100 years
- Uses the right language for each subsystem: **Rust** for runtime/safety, **Python** for research/training, **C++** for driver integration

## 2. Design Principles

1. **Language as a tool, not a religion**: Each language owns the subsystem where it has the highest signal-to-noise ratio.
2. **Safety first**: Rust owns all runtime model inference, serialization, and state management. No undefined behavior in the hot path.
3. **Performance with observability**: Every decision is logged, explainable, and auditable. No black-box ML.
4. **Energy-aware optimization**: The primary objective function is **performance per watt**, not raw throughput.
5. **Continual learning**: Models improve over time without catastrophic forgetting. No offline retraining required.
6. **Modularity**: Models, optimizers, and policies are pluggable. Adding a new model type should not touch the scheduler.
7. **Versioned evolution**: All model configs, weights, and schemas are versioned. Old models can be migrated or retired gracefully.
8. **Cross-platform**: Works on Windows (MSVC), Linux (Clang/GCC), and eventually NixOS.

## 3. System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Synapse ML Polyglot Stack                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐     │
│  │   Vulkan     │    │   D3D12      │    │   WAL /      │     │
│  │   Layer      │    │   Layer      │    │   Recovery   │     │
│  │  (C++)       │    │  (C++)       │    │   (Rust)     │     │
│  └──────┬───────┘    └──────┬───────┘    └──────┬───────┘     │
│         │                   │                   │              │
│         └───────────────────┼───────────────────┘              │
│                             │                                  │
│                    ┌────────▼────────┐                        │
│                    │   C++ FFI       │                        │
│                    │   (cbindgen)    │                        │
│                    └────────┬────────┘                        │
│                             │                                  │
│  ┌──────────────────────────▼──────────────────────────┐     │
│  │          SynapseCore (C++)                           │     │
│  │  - Workload capture                                 │     │
│  │  - Feature extraction via FeatureEncoder             │     │
│  │  - Backend selection via MLSubAPI                    │     │
│  │  - Outcome observation + reward calculation          │     │
│  └──────────────────────────┬──────────────────────────┘     │
│                             │                                  │
│                    ┌────────▼────────┐                        │
│                    │  C ABI FFI      │                        │
│                    │  (synapse_ml.h) │                        │
│                    └────────┬────────┘                        │
│                             │                                  │
│  ┌──────────────────────────▼──────────────────────────┐     │
│  │          igpu_ml_core (Rust)                         │     │
│  │  - Model trait registry                             │     │
│  │  - Inference engine                                 │     │
│  │  - Experience replay buffer                         │     │
│  │  - Continual learning loop                          │     │
│  │  - Energy-aware loss functions                      │     │
│  │  - Telemetry-driven optimization                    │     │
│  └──────────────────────────┬──────────────────────────┘     │
│                             │                                  │
│         ┌───────────────────┼───────────────────┐            │
│         │                   │                   │            │
│  ┌──────▼──────┐    ┌──────▼──────┐    ┌──────▼──────┐     │
│  │ igpu_ml_   │    │ igpu_ml_   │    │ igpu_ml_   │      │
│  │ models     │    │ optim      │    │ python     │      │
│  │ (Rust)     │    │ (Rust)     │    │ (PyO3)     │      │
│  └────────────┘    └────────────┘    └────────────┘      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 4. Core Abstractions

### 4.1 Model Trait

All models implement a single, stable trait:

```rust
pub trait Model: Send + Sync {
    /// Unique model identifier, versioned (e.g., "dynamic-tile-v1")
    fn id(&self) -> &str;

    /// Current model version (semantic)
    fn version(&self) -> &str;

    /// Inference: given features, return action scores
    fn infer(&self, features: &FeatureVector) -> ActionScores;

    /// Online update: learn from experience
    fn update(&mut self, experience: &Experience) -> UpdateResult;

    /// Batch update: gradient step from replay buffer
    fn batch_update(&mut self, batch: &[Experience]) -> BatchResult;

    /// Serialize model weights/config to bytes
    fn serialize(&self) -> Vec<u8>;

    /// Deserialize and replace current state
    fn deserialize(&mut self, bytes: &[u8]) -> Result<()>;

    /// Model-specific metadata for explainability
    fn explain(&self, features: &FeatureVector) -> ModelExplanation;
}
```

### 4.2 Feature Vector

```rust
#[repr(C)]
#[derive(Clone, Copy, Debug, Serialize, Deserialize)]
pub struct FeatureVector {
    pub shader_complexity_norm: f32,      // 0..1
    pub vertex_count_log_norm: f32,       // 0..1
    pub draw_call_rate_norm: f32,         // 0..1
    pub cache_hit_rate: f32,              // 0..1
    pub dvfs_headroom: f32,               // 0..1
    pub thermal_headroom: f32,            // 0..1
    pub predictor_accuracy: f32,          // 0..1
    pub is_compute_dispatch: f32,         // 0 or 1
}
```

### 4.3 Action Space

```rust
#[repr(C)]
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub enum Backend {
    Jit = 0,
    Hai = 1,
    Oracle = 2,
    // Extensible: future backends can be added without breaking ABI
}

#[repr(C)]
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ActionScores {
    pub scores: [f32; 3],  // JIT, HAI, Oracle
    pub chosen: Backend,
    pub confidence: f32,    // max(scores) - mean(scores)
    pub epsilon_greedy: bool,
}
```

### 4.4 Experience

```rust
#[repr(C)]
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Experience {
    pub features: FeatureVector,
    pub action: Backend,
    pub reward: f32,
    pub timestamp_ns: u64,
    pub session_id: u64,
}
```

## 5. Module Specifications

### 5.1 igpu_ml_core

**Responsibility**: Stable ABI, model registry, inference engine, replay buffer, training loop.

**Key types**:
- `Model` trait
- `ModelRegistry`: maps model ID → boxed dyn Model
- `ReplayBuffer`: lock-free ring buffer for experiences
- `Trainer`: orchestrates batch updates, learning rate scheduling, checkpointing
- `InferenceEngine`: thread-safe inference with caching

**FFI exports**:
```c
// synapse_ml.h - auto-generated by cbindgen
int32_t synapse_ml_init(void);
void synapse_ml_shutdown(void);
int32_t synapse_ml_register_model(const char* id, const char* version, const uint8_t* config, size_t config_len);
int32_t synapse_ml_infer(const char* model_id, const SynapseFeatureVector* features, SynapseActionScores* out);
int32_t synapse_ml_observe(const char* model_id, const SynapseExperience* experience);
int32_t synapse_ml_checkpoint(const char* model_id, const char* path);
int32_t synapse_ml_restore(const char* model_id, const char* path);
void synapse_ml_free_string(char* s);
```

### 5.2 igpu_ml_models

**Responsibility**: Concrete model implementations.

**First model: DynamicTileTransformer**

Design rationale for iGPUs:
- iGPUs have **limited shared memory** (often 128KB–512KB per subslice)
- **Tile-based deferred rendering** (TBDR) is common on mobile/embedded GPUs
- **Bandwidth** is the primary bottleneck, not compute
- **Power/thermal** constraints limit sustained boost clocks

```rust
pub struct DynamicTileTransformer {
    // Input: 8-dimensional feature vector
    // Hidden layers: 32 → 16 → 8 (small enough for iGPU L1 cache)
    // Output: 3 action scores
    weights: Vec<f32>,
    biases: Vec<f32>,
    learning_rate: f32,
    epsilon: f32,
    rng: StdRng,
    version: String,
}

impl Model for DynamicTileTransformer {
    fn infer(&self, features: &FeatureVector) -> ActionScores {
        // Forward pass: 8 → 32 → 16 → 8 → 3
        // Use ReLU for hidden layers, linear for output
        // Cache hot path: precompute common feature patterns
    }

    fn update(&mut self, experience: &Experience) -> UpdateResult {
        // Online gradient update with energy-aware loss
        let predicted = self.infer(&experience.features);
        let target = experience.reward;
        // MSE loss + L2 regularization
        // Clip gradients to prevent explosion
    }
}
```

**Model config format**:
```json
{
  "model_type": "dynamic-tile-transformer",
  "version": "1.0.0",
  "input_dim": 8,
  "hidden_dims": [32, 16, 8],
  "output_dim": 3,
  "activation": "relu",
  "learning_rate": 0.01,
  "epsilon": 0.05,
  "l2_lambda": 0.001,
  "energy_weight": 0.7,
  "performance_weight": 0.3
}
```

### 5.3 igpu_ml_optim

**Responsibility**: Optimization algorithms, energy-aware loss functions, learning rate schedules.

**Key components**:
- `EnergyAwareLoss`: combines performance reward with energy penalty
- `AdaptiveLearningRate`: schedules LR based on convergence signals
- `PolicyGradient`: REINFORCE-style updates for stochastic policies
- `MCTS`: optional Monte Carlo Tree Search for long-horizon planning (future)

```rust
pub struct EnergyAwareLoss {
    pub energy_weight: f32,      // λ_energy: prioritize energy savings
    pub performance_weight: f32,  // λ_perf: prioritize throughput
    pub thermal_penalty: f32,    // penalty factor for thermal events
}

impl EnergyAwareLoss {
    pub fn compute(&self, reward: f32, energy_joules: f32, thermal_events: u32) -> f32 {
        let energy_term = self.energy_weight * (1.0 / (1.0 + energy_joules));
        let perf_term = self.performance_weight * reward;
        let thermal_term = self.thermal_penalty * (thermal_events as f32);
        energy_term + perf_term - thermal_term
    }
}
```

### 5.4 igpu_ml_python

**Responsibility**: Python bindings for training, experimentation, and tooling.

**Bindings**:
```python
import igpu_ml

# Create model
model = igpu_ml.ModelRegistry.create("dynamic-tile-transformer", version="1.0.0")

# Infer
features = igpu_ml.FeatureVector(
    shader_complexity_norm=0.7,
    vertex_count_log_norm=0.3,
    draw_call_rate_norm=0.1,
    cache_hit_rate=0.89,
    dvfs_headroom=0.6,
    thermal_headroom=0.8,
    predictor_accuracy=0.93,
    is_compute_dispatch=0.0,
)
scores = model.infer(features)
print(f"Chosen backend: {scores.chosen}, confidence: {scores.confidence}")

# Observe
experience = igpu_ml.Experience(
    features=features,
    action=igpu_ml.Backend.JIT,
    reward=0.85,
    timestamp_ns=1234567890,
    session_id=42,
)
model.observe(experience)

# Checkpoint
model.checkpoint("model_v1.bin")
```

## 6. Data Flow

```
┌─────────────┐
│ Vulkan/D3D12│
│   Layer     │
│  (C++)      │
└──────┬──────┘
       │ WorkloadSignature
       ▼
┌───────────────────┐
│ FeatureEncoder    │
│ (C++ → Rust FFI)  │
└──────┬────────────┘
       │ FeatureVector
       ▼
┌───────────────────┐
│ InferenceEngine   │
│ (Rust)            │
│ - Model lookup    │
│ - Batch inference │
│ - Caching         │
└──────┬────────────┘
       │ ActionScores
       ▼
┌───────────────────┐
│ Backend Selector  │
│ (C++)             │
│ - Epsilon-greedy  │
│ - Confidence gate │
└──────┬────────────┘
       │ ExecutionBackend
       ▼
┌───────────────────┐
│ Driver Dispatch   │
│ (C++/Driver)      │
└──────┬────────────┘
       │ Outcome + Latency + Energy
       ▼
┌───────────────────┐
│ Reward Calculator │
│ (Rust)            │
│ - Performance     │
│ - Energy (proxy)  │
│ - Thermal penalty │
└──────┬────────────┘
       │ Experience
       ▼
┌───────────────────┐
│ ReplayBuffer      │
│ (Rust)            │
│ - Ring buffer     │
│ - Priority sample │
└──────┬────────────┘
       │ Batch
       ▼
┌───────────────────┐
│ Trainer           │
│ (Rust)            │
│ - Batch update    │
│ - LR schedule     │
│ - Checkpoint      │
└───────────────────┘
```

## 7. Continual Learning Strategy

### 7.1 Experience Replay
- Ring buffer of 65536 experiences (configurable)
- Priority sampling: experiences with high temporal difference error are sampled more frequently
- Prevents catastrophic forgetting by maintaining distributional stability

### 7.2 Online Learning
- Each inference→observation cycle updates the model incrementally
- Learning rate decays over time: `lr(t) = lr0 / (1 + decay * t)`
- Epsilon decays: `ε(t) = ε0 * exp(-decay * t)`
- Freeze/unfreeze API for safe model swaps

### 7.3 Model Versioning
```json
{
  "model_id": "dynamic-tile-transformer",
  "version": "1.2.0",
  "created_at": "2026-08-12T00:00:00Z",
  "parent_version": "1.1.0",
  "training_steps": 150000,
  "validation_accuracy": 0.94,
  "config_hash": "sha256:abc123...",
  "weights_hash": "sha256:def456..."
}
```

### 7.4 A/B Testing
- Multiple models can run in parallel
- Traffic split configurable: `{"model_A": 0.8, "model_B": 0.2}`
- Automatic promotion of better-performing models based on rolling reward

## 8. Energy-Aware Optimization

### 8.1 Primary Objective Function

```
L_total = λ_perf * L_perf + λ_energy * L_energy + λ_thermal * L_thermal + λ_reg * L_reg
```

Where:
- `L_perf`: negative reward (lower is better) = `-throughput / power`
- `L_energy`: energy consumption in joules (from DVFS telemetry)
- `L_thermal`: thermal mitigation events (penalty)
- `L_reg`: L2 regularization on weights

### 8.2 Proxy Metrics
Since direct power measurement is hardware-dependent, use proxies from `SynapseSessionReport`:
- `dvfs.total_transitions` → energy instability
- `thermal.thermal_mitigation_events` → thermal pressure
- `power.joules_saved` → direct energy feedback (when available)
- `backend_routing.jit_dispatches` vs `hai_dispatches` → compression efficiency

## 9. FFI Contract

### 9.1 Rust → C++ (cbindgen)

```rust
// rust/crates/igpu_ml_core/src/ffi.rs
#[repr(C)]
#[derive(Serialize, Deserialize)]
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
#[derive(Serialize, Deserialize)]
pub struct SynapseActionScores {
    pub scores: [f32; 3],
    pub chosen: u32,  // Backend as u32
    pub confidence: f32,
    pub epsilon_greedy: bool,
}

#[repr(C)]
#[derive(Serialize, Deserialize)]
pub struct SynapseExperience {
    pub features: SynapseFeatureVector,
    pub action: u32,
    pub reward: f32,
    pub timestamp_ns: u64,
    pub session_id: u64,
}

#[no_mangle]
pub extern "C" fn synapse_ml_init() -> i32 { ... }

#[no_mangle]
pub extern "C" fn synapse_ml_infer(
    model_id: *const u8,
    model_id_len: u32,
    features: *const SynapseFeatureVector,
    out_scores: *mut SynapseActionScores,
) -> i32 { ... }
```

### 9.2 Rust → Python (PyO3)

```python
# igpu_ml_python bindings
#[pymodule]
fn igpu_ml(_py: Python, m: &PyModule) -> PyResult<()> {
    m.add_class::<FeatureVector>()?;
    m.add_class::<ActionScores>()?;
    m.add_class::<Experience>()?;
    m.add_class::<ModelRegistry>()?;
    Ok(())
}
```

## 10. Testing Strategy

### Rust Unit Tests
- Model inference correctness (deterministic with fixed RNG seed)
- Update rule convergence (synthetic rewards)
- Serialization round-trip
- Replay buffer overflow/underflow

### Rust Property Tests
- Feature vector normalization stays in [0, 1]
- Action scores sum to valid probability distribution
- Model version increments monotonically

### C++ Integration Tests
- FFI boundary: infer/observe round-trip
- C++ calls Rust infer, gets valid backend choice
- C++ observes outcome, Rust model updates

### Python Integration Tests
- PyO3 bindings: create model, infer, observe, checkpoint
- Training script produces valid model file

### Cross-Language CI
```bash
# scripts/test_all.sh
set -e
cd "$(dirname "$0")/.."

echo "[1/3] Rust tests..."
cd rust && cargo test --all && cd ..

echo "[2/3] C++ tests..."
cd build/cpp && ctest --output-on-failure && cd ../..

echo "[3/3] Python tests..."
cd python && pytest && cd ..

echo "=== All polyglot ML tests passed ==="
```

## 11. Performance Targets

| Metric | Target | Measurement |
|--------|--------|-------------|
| Inference latency | < 1 µs per call | Rust benchmark, no heap alloc |
| Batch update (32 experiences) | < 100 µs | Rust benchmark |
| Model serialization | < 10 ms for 1MB weights | Rust benchmark |
| Memory footprint | < 2 MB per model | Static analysis |
| Replay buffer throughput | > 1M experiences/sec | Rust benchmark with Rayon |
| Python training iteration | < 10 ms | PyO3 overhead included |

## 12. Roadmap

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| **0** | 1 week | Scaffold Rust workspace, CMake integration, cbindgen |
| **1** | 2 weeks | Core trait + registry + FFI + C++ integration |
| **2** | 2 weeks | DynamicTileTransformer model + inference benchmark |
| **3** | 2 weeks | Continual learning engine + replay buffer |
| **4** | 1 week | Energy-aware loss + telemetry-driven optimizer |
| **5** | 1 week | PyO3 bindings + Python training harness |
| **6** | 1 week | Model versioning + checkpointing + A/B testing |
| **7** | Ongoing | Dashboard integration, model zoo expansion, auto-update pipeline |

## 13. Success Criteria

1. **Stability**: Zero panics/safety violations in Rust runtime under 72-hour stress test
2. **Performance**: Inference latency < 1 µs, batch update < 100 µs
3. **Efficiency**: 10%+ improvement in performance-per-watt vs baseline heuristic
4. **Modularity**: New model can be added by implementing `Model` trait only
5. **Scalability**: Replay buffer handles 1M+ experiences without allocation failures
6. **Explainability**: Every decision can be traced to feature weights and model version
7. **Evolution**: Models can be updated without restarting the Vulkan layer
8. **Cross-platform**: Builds and tests pass on Windows (MSVC) and Linux (Clang 17)
