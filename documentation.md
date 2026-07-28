# Project Synapse — Technical Reference Documentation

**Version:** 1.0.0  
**Date:** March 5, 2026  
**Classification:** Internal Engineering Reference  
**Single-Sentence Purpose:** Project Synapse is a hybrid, adaptive software shim that intercepts Vulkan/D3D12 command streams in the User-Mode Driver to dynamically route work between JIT compilation, HAI bytecode streaming, and Oracle fallback — delivering measurable reductions in driver CPU overhead and memory bandwidth consumption on iGPU hardware.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Module Reference](#module-reference)
   - [SynapseCore](#synapsecore)
   - [Analyzer & TelemetryRingBuffer](#analyzer--telemetryringbuffer)
   - [Scheduler](#scheduler)
   - [JITPipeline & JITSpecializationCache](#jitpipeline--jitspecializationcache)
   - [HAIFrontendSim & SynapseHAIBuilder](#haifrontendsim--synapsehaibilder)
   - [DVFSController](#dvfscontroller)
   - [PowerEstimator](#powerestimator)
   - [PredictiveEngine](#predictiveengine)
   - [ITSCacheController](#itscachecontroller)
   - [TextureStreamingEngineHardened](#texturestreamingengineHardened)
   - [PushConstantOptimizer](#pushconstantoptimizer)
   - [DescriptorTracker](#descriptortracker)
   - [SyncManager & FenceManager](#syncmanager--fencemanager)
   - [ConfidenceAggregator](#confidenceaggregator)
   - [ForecastingProfiler](#forecastingprofiler)
   - [SmoothingEngine (PGRO)](#smoothingengine-pgro)
   - [ThermalAwareArbiter](#thermalawarearbiter)
3. [Data Flows](#data-flows)
4. [Telemetry Reference](#telemetry-reference)
5. [Edge Cases & Fallback Behaviors](#edge-cases--fallback-behaviors)
6. [Canonical Data Structures](#canonical-data-structures)
7. [API Contracts](#api-contracts)
8. [Scope Limits](#scope-limits)

---

## Architecture Overview

Project Synapse inserts itself as a transparent layer inside the User-Mode Driver (UMD). Every Vulkan command that touches GPU resources — `vkCmdDrawIndexed`, `vkCmdDraw`, `vkCmdDispatch`, `vkCmdPushConstants`, `vkCmdBindDescriptorSets` — is intercepted before it reaches the native ISA generator. The shim hooks `vkCreateImage`/`vkDestroyImage` to track texture residency for ITS and `vkCmdBindShadersEXT` for VK_EXT_shader_object JIT submissions. The shim performs real-time telemetry collection, workload classification, and execution routing in under 1 microsecond of additional latency on the critical path.

```
Application (Game / Compute Workload)
         │
         │  vkCmdDraw*, vkCmdDispatch, vkCmdPushConstants
         ▼
┌─────────────────────────────────────────────────────────┐
│                  SYNAPSE SHIM LAYER (UMD)               │
│                                                         │
│  ┌──────────────┐   push    ┌───────────────────────┐   │
│  │  SynapseCore │ ────────► │ TelemetryRingBuffer   │   │
│  │  (Intercept) │           │ (Lock-Free SPSC)      │   │
│  └──────┬───────┘           └──────────┬────────────┘   │
│         │                             pop               │
│         │                   ┌──────────▼────────────┐   │
│         │                   │  Analyzer             │   │
│         │                   │  (Background Thread)  │   │
│         │                   └──────────┬────────────┘   │
│         │                             │ recommendation   │
│         │                   ┌──────────▼────────────┐   │
│         │                   │  Scheduler            │   │
│         │                   │  (Backend Decision)   │   │
│         │                   └──────────┬────────────┘   │
│         │                             │                  │
│    ┌────▼──────┬──────────────────────┤                  │
│    │ JIT Path  │   HAI Path           │ Oracle Path      │
│    │JITPipeline│  HAIFrontendSim      │ (native passthru)│
│    └─────┬─────┴───────────┬──────────┘                  │
│          │                 │                             │
│  ┌───────▼──────────────────▼──────────────────────────┐ │
│  │  SyncManager / FenceManager  (Safety Gate)          │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                         │
│  ┌──────────────────────┐  ┌──────────────────────────┐  │
│  │ DVFSController       │  │ ThermalAwareArbiter       │  │
│  │ PowerEstimator       │  │ SmoothingEngine (PGRO)    │  │
│  └──────────────────────┘  └──────────────────────────┘  │
│                                                         │
│  ┌──────────────────────┐  ┌──────────────────────────┐  │
│  │ PredictiveEngine     │  │ DescriptorTracker         │  │
│  │ ITSCacheController   │  │ PushConstantOptimizer     │  │
│  │ ConfidenceAggregator │  │ ForecastingProfiler       │  │
│  └──────────────────────┘  └──────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
         │
         ▼
   Native iGPU Hardware (PMU / DMA Engine / Command Processor)
```

### Execution Backend Decision Matrix

| Workload Type              | Condition                                      | Selected Backend |
|:---------------------------|:-----------------------------------------------|:-----------------|
| Complex geometry shader    | `shader_instruction_estimate > 256`, stable     | JIT              |
| Particle / UI draw calls   | `draw_call_count > 100`, low state changes     | HAI              |
| Heavy compute dispatch     | `is_compute_dispatch == true`, high confidence | JIT              |
| Unknown / cold start       | Analyzer not yet warmed up                     | Oracle           |
| Prediction failures        | JIT occupancy < threshold or stall detected    | Oracle (fallback)|
| Thermal mitigation active  | `thermal_headroom < 0.20f`                     | HAI (mip-capped) |

---

## Module Reference

---

### SynapseCore

**File:** [synapse/synapse_core.h](synapse/synapse_core.h), [synapse/synapse_core.cpp](synapse/synapse_core.cpp)  
**Namespace:** `synapse`  
**Responsibility (SRP):** Central coordinator. Owns the UMD hook interception point, routes each draw call to the correct execution backend, and manages the lifecycle of the background Analyzer thread.

#### Public Interface

```cpp
SynapseCore(PFN_vkCmdDrawIndexed orig_draw,
            PFN_vkCmdDraw orig_draw_non_indexed,
            PFN_vkCmdDispatch orig_dispatch,
            PFN_vkCmdPushConstants orig_push_constants,
            PFN_vkCmdBindDescriptorSets orig_bind_desc_sets,
            PFN_vkCmdBindShadersEXT orig_bind_shaders);
~SynapseCore();

void handle_draw_indexed(VkCommandBuffer cmd, uint32_t indexCount,
                          uint32_t instanceCount, uint32_t firstIndex,
                          int32_t vertexOffset = 0, uint32_t firstInstance = 0);
void handle_draw(VkCommandBuffer cmd, uint32_t vertexCount,
                  uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
void handle_dispatch(VkCommandBuffer cmd, uint32_t groupCountX,
                     uint32_t groupCountY, uint32_t groupCountZ);
void handle_push_constants(VkCommandBuffer cmd, VkPipelineLayout layout,
                            uint32_t offset, uint32_t size, const void* pValues);
void handle_bind_descriptor_sets(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint,
                                  VkPipelineLayout layout, uint32_t firstSet,
                                  uint32_t descriptorSetCount,
                                  const VkDescriptorSet* pDescriptorSets,
                                  uint32_t dynamicOffsetCount,
                                  const uint32_t* pDynamicOffsets);
```

#### Critical Path Sequence (per draw call)

1. `capture_current_signature(cmd, indexCount)` → produces `WorkloadSignature`
2. `telemetry_.push(sig)` — lock-free push to ring buffer (≤ 10 ns)
3. `its_engine_.prepare_for_use(image, frame)` — ITS residency check
4. `scheduler_.decide_backend(sig)` → `ExecutionBackend` enum
5. Dispatch to `execute_jit_path()`, `execute_hai_path()`, or native Oracle

#### Context Hash Formula

The JIT context hash uses a modified Boost hash_combine to avoid collisions between shader identity and workload state:

$$H(s, w) = H(s) \oplus (H(w) + 0x9e3779b9 + (H(s) \ll 6) + (H(s) \gg 2))$$

#### Power-Performance Conflict Resolution

`resolve_power_perf_conflict()` (in [synapse_core.cpp](synapse/synapse_core.cpp)) is called by the `PolicyArbiter` when `SmoothingEngine` signals `is_stability_critical()`. When stability is at risk:
- Calls `dvfs_controller_.force_performance_state(PState::F0_MAX)`
- Increments `stats_.stability_overrides_count`
- Latches `was_previously_locked_ = true` to prevent repeated stat increments

---

### Analyzer & TelemetryRingBuffer

**File:** [synapse/synapse_umd.h](synapse/synapse_umd.h)  
**Namespace:** `synapse`  
**Responsibility:** `TelemetryRingBuffer` is a lock-free, single-producer/single-consumer ring buffer that decouples the render thread (producer) from the Analyzer thread (consumer). `Analyzer` consumes workload signatures, builds a rolling model, and surfaces a `current_recommendation()` and `get_last_known_signature()`.

#### TelemetryRingBuffer Contract

```cpp
bool push(const WorkloadSignature& sample) noexcept;   // Producer (render thread)
std::optional<WorkloadSignature> pop() noexcept;        // Consumer (Analyzer thread)
```

- **Buffer size:** 1024 entries (power of two, wraps with `& (kBufferSize - 1)`)
- **Memory ordering:** `head_` uses `release` on write, `acquire` on read. `tail_` uses symmetric semantics.
- **False sharing prevention:** `head_` and `tail_` are each aligned to `kCacheLineSize` (64 bytes or `std::hardware_destructive_interference_size`)
- **Overflow behavior:** `push()` returns `false` (non-blocking). The render thread DOES NOT stall.

#### WorkloadSignature Fields

| Field                        | Type       | Description                                      |
|:-----------------------------|:-----------|:-------------------------------------------------|
| `draw_call_count`            | `uint32_t` | Number of draws in the current batch             |
| `pipeline_state_changes`     | `uint32_t` | PSO switches — high value favors HAI             |
| `shader_instruction_estimate`| `uint32_t` | Instruction density estimate — high favors JIT   |
| `vertex_count`               | `uint32_t` | Geometry throughput indicator                    |
| `texture_bindings`           | `uint32_t` | Memory bandwidth pressure indicator              |
| `is_compute_dispatch`        | `bool`     | `true` if this is a compute rather than graphics |

---

### Scheduler

**File:** [synapse/synapse_umd.h](synapse/synapse_umd.h)  
**Namespace:** `synapse`  
**Responsibility:** Consumes the Analyzer's current recommendation and maps it to an `ExecutionBackend` enum value. Owns Oracle Mode detection and fallback logic.

#### ExecutionBackend Enum

| Value     | Meaning                                              |
|:----------|:-----------------------------------------------------|
| `JIT`     | Offload to JIT specializing compiler                 |
| `HAI`     | Stream compressed HAI bytecode to hardware frontend  |
| `Oracle`  | Pass-through to native driver (zero Synapse overhead)|

#### Oracle Mode Rules
- Activated on cold start (Analyzer not yet converged)
- Activated on detected GPU pipeline stall caused by a misprediction
- Activated when `JITPipeline::get_optimized_shader()` returns `nullptr`
- Deactivated automatically after Analyzer confidence exceeds threshold (`T_HIGH = 0.82f`)

---

### JITPipeline & JITSpecializationCache

**Files:** [synapse/synapse_jit_backend.h](synapse/synapse_jit_backend.h), [synapse/jit_specialization_cache.h](synapse/jit_specialization_cache.h)  
**Namespace:** `synapse`  
**Responsibility:** `JITPipeline` compiles or retrieves telemetry-specialized ISA for a given shader. `JITSpecializationCache` stores compiled `SpecializedShader` objects with lock-free read semantics for the render thread.

#### JITPipeline Compilation Steps

1. **Telemetry Constant Injection (PGRO):** If `shader_instruction_estimate` is high, prioritize register reuse via `inject_telemetry_constants()`
2. **Register Pressure Reduction (RPR):** Live-range splitting to hit target register count
3. **Occupancy Calculation:** $\text{Occupancy} = \lfloor 1024 / \text{reg\_count} \rfloor$ — targets 256KB VGPR file model
4. **ISA Generation:** Produces `SpecializedShader.isa_binary` (`std::vector<uint32_t>`)

#### JITSpecializationCache Thread Safety

- **Render thread (fast path):** `get(hash)` → `std::atomic<shared_ptr>::load(acquire)` — zero locks
- **Analyzer thread (background):** `insert(hash, shader)` → `store(release)` — zero locks
- **Cache size:** 1024 slots (fixed-size array, hash-indexed)

#### Defensive Behavior

- `compile_specialized()` returns `nullptr` if `source.empty()` — triggers Oracle fallback in `SynapseCore`
- Cache lookup always checked before compilation to prevent redundant work

---

### HAIFrontendSim & SynapseHAIBuilder

**Files:** [synapse/hai_frontend_sim.h](synapse/hai_frontend_sim.h), [synapse/synapse_hai_builder.h](synapse/synapse_hai_builder.h)  
**Namespace:** `synapse::sim`, `synapse`  
**Responsibility:** `HAIFrontendSim` is the cycle-approximate simulator of the hardware bytecode decoder. `SynapseHAIBuilder` constructs the compressed bytecode stream for a batch of draw calls.

#### HAI Bytecode Instruction Format

```
Byte 0-1:  Descriptor  [opcode:6 | length:4 | reserved:6]
Byte 2+:   Payload     [length * 4 bytes]
```

#### Key Opcodes

| Opcode | Mnemonic         | Purpose                                           |
|:-------|:-----------------|:--------------------------------------------------|
| `0xFF` | `DELTA_UPDATE`   | Update only changed fields via bitmask            |
| `0x30` | `REPEAT`         | Re-submit last full command with no changes       |
| `0x50` | `SET_EXPECTED_LOAD` | Proactive scheduler hint (PGRO emission)       |
| `0x12` | `SET_PUSH_CONSTANT` | Push constant full or delta set                |

#### Delta Update Compression

For `DELTA_UPDATE`: the 16-bit dirty bitmask determines payload size.
$$\text{PayloadBytes} = \text{popcount}(\text{mask}) \times 4$$

When only `indexCount` changes frame-over-frame, the entire draw submission is 7 bytes vs. the standard 48-byte full draw command — a **6.8x compression ratio** for static scene geometry.

#### HAIFrontendSim SimulationMetrics

| Field                      | Description                                            |
|:---------------------------|:-------------------------------------------------------|
| `total_cycles`             | Simulated cycles to decode the batch                   |
| `total_bytes_fetched`      | Actual bytes consumed from the bytecode stream         |
| `delta_updates_processed`  | Count of DELTA_UPDATE opcodes decoded                  |
| `cache_hits / misses`      | Shadow State Register (SSR) hit/miss counters          |
| `calculate_compression_ratio(raw)` | `raw / total_bytes_fetched` — target > 4.0   |

---

### DVFSController

**Files:** [synapse/dvfs_controller.h](synapse/dvfs_controller.h), [synapse/dvfs_controller.cpp](synapse/dvfs_controller.cpp)  
**Namespace:** `synapse::power`  
**Responsibility:** Manages iGPU frequency/voltage P-State transitions. Enforces hysteresis to prevent oscillation and coordinates with `SyncManager` during transitions.

#### P-State Definitions

| PState         | Description                  | Use Case                        |
|:---------------|:-----------------------------|:--------------------------------|
| `F0_MAX`       | Maximum voltage/frequency    | Heavy geometry, compute shaders |
| `F1_BALANCED`  | Mid-range (default)          | Mixed workload                  |
| `F2_EFFICIENT` | Minimum voltage/frequency    | UI rendering, idle frames       |

#### Transition Protocol

1. `update_policy(current_us, predicted_mb_s)` — evaluates demand, skips if `TRANSITIONING`
2. `initiate_transition(target, start_us)` — locks state for **75 µs**, registers a global bus lock with `SyncManager`
3. During lock: all draw submissions from `SyncManager` are stalled until `transition_end_us_`
4. Energy cost of switch: **150 nJ** logged to `PowerEstimator`
5. `complete_transition()` — sets `current_p_state_`, returns to `STEADY`

#### Hysteresis Rule

A P-State change is only applied if `frames_since_switch_ > 5` (approximately 83 ms at 60 FPS), preventing oscillation for borderline workloads. Emergency overrides via `handle_sync_stall()` bypass hysteresis entirely.

---

### PowerEstimator

**File:** [synapse/power_estimator.h](synapse/power_estimator.h)  
**Namespace:** `synapse::metrics`  
**Responsibility:** Accumulates energy telemetry across all transactions and generates battery impact reports.

#### Energy Model

Baseline constant (2026 LPDDR5): `PJ_PER_BIT = 35.0 pJ`

$$E_{potential} = \text{full\_bytes} \times 8 \times 35.0 \text{ pJ}$$
$$E_{actual} = \text{actual\_bytes} \times 8 \times 35.0 \text{ pJ}$$
$$E_{saved} = E_{potential} - E_{actual}$$

At 60 FPS (frame time ≈ 16.6 ms):

$$P_{saved} = \frac{E_{saved}[\text{J}]}{\text{frames} \times 0.0166 \text{ s}} \times 1000 \text{ mW}$$

#### PowerReport Fields

| Field                       | Description                                           |
|:----------------------------|:------------------------------------------------------|
| `joules_saved`              | Total energy saved over the session (Joules)          |
| `avg_milliwatts_saved_at_60fps` | Average continuous power reduction (mW)           |
| `battery_extension_factor`  | Ratio: `E_saved / E_potential` — theoretical max gain |

---

### PredictiveEngine

**File:** [synapse/predictive_engine.h](synapse/predictive_engine.h)  
**Namespace:** `synapse::its`  
**Responsibility:** Predicts future resource residency requirements using temporal locality heuristics. Emits prefetch commands via `HAIBytecodeBuilder` and registers DMA fences via `SyncManager`.

#### Prediction Algorithm

For every resource bound in the current frame:
1. **Hit tracking:** If `resource_id` is in `active_predictions_`, increment `accurate_predictions`
2. **Temporal locality rule:** Resource used now → will be used in the next `temporal_window_` frames
3. **Mip targeting:** Request `resident_mips - 1` (next higher detail level)
4. **Cache check:** `ITSCacheController::access_resource()` → on miss, emit prefetch + fence
5. **Prediction pruning:** Evict predictions older than `temporal_window_ + 5` frames as waste

#### Temporal Window Configuration

Default `temporal_window_frames = 3`. The `ForecastingProfiler` evaluates horizons at 5, 10, 20, and 30 frames to determine the optimal window per workload type.

#### PredictionStats (Telemetry)

| Field                 | Baseline (report.json) | Acceptance Threshold |
|:----------------------|:-----------------------|:---------------------|
| `total_predictions`   | 8,450                  | —                    |
| `accurate_predictions`| 7,900                  | —                    |
| `wasted_predictions`  | 550                    | ≤ 7% of total        |
| `accuracy_rate`       | **93.4%**              | ≥ 90%                |

---

### ITSCacheController

**File:** [synapse/its_cache_controller.h](synapse/its_cache_controller.h)  
**Namespace:** `synapse::its`  
**Responsibility:** Manages the Integrated Texture Streaming (ITS) cache with strict byte-capacity enforcement and LRU eviction. Thread-safe for async prefetch queue integration.

#### LRU Policy

- `access_resource(id, bytes)` → cache hit: move to MRU front, return `true`; cache miss: evict LRU tail until capacity allows, insert, return `false`
- `remove_resource(id)` → called on `vkDestroyImage` / buffer destruction
- `ensure_capacity_locked()` performs tail eviction while `current_usage_ + required > capacity_`

#### Telemetry API

| Method              | Returns  | Description                          |
|:--------------------|:---------|:-------------------------------------|
| `get_hits()`        | `uint64_t` | Total cache hits since creation      |
| `get_misses()`      | `uint64_t` | Total cache misses since creation    |
| `get_hit_rate()`    | `float`  | `hits / (hits + misses)` — target ≥ 0.89 |
| `get_current_usage()` | `uint64_t` | Live byte residency footprint      |

**Baseline from report.json:** hit rate = **89%**, sync stalls = 12.

---

### TextureStreamingEngineHardened

**File:** [synapse/its_engine_hardened.h](synapse/its_engine_hardened.h)  
**Namespace:** `synapse`  
**Responsibility:** Manages per-mip residency states for all tracked textures with hysteresis-gated load/evict decisions and DMA fence validation before declaring a mip safe for shader access.

#### Hysteresis Thresholds

| Threshold        | Value  | Action Triggered                            |
|:-----------------|:-------|:--------------------------------------------|
| `kLoadThreshold` | 0.85   | Trigger async mip load via DMA              |
| `kEvictThreshold`| 0.15   | Trigger async mip eviction                  |

Demands in the range (0.15, 0.85) produce no state change — preventing residency flickering in borderline workloads.

#### Safe Mip Level Query

`get_safe_mip_level(image)` iterates from mip 0 (highest detail) downward, returning the first mip where:
- `is_resident == true` (atomic load)
- `hardware_fence_completed(dma_fence_id) == true`

If no mip satisfies both conditions, returns `mip_count - 1` (lowest detail fallback — never produces tearing).

---

### PushConstantOptimizer

**File:** [synapse/push_constant_optimizer.h](synapse/push_constant_optimizer.h)  
**Namespace:** `synapse::optimizer`  
**Responsibility:** Intercepts all `vkCmdPushConstants` calls, computes a word-level dirty bitmask against the per-command-buffer shadow state, and emits either a full `SET_PUSH_CONSTANT` or a `DELTA_UPDATE` HAI instruction.

#### Delta Decision Rule

| Condition                                        | HAI Emission                        |
|:-------------------------------------------------|:------------------------------------|
| First update (invalidated) OR >50% words changed | Full `emit_set_push_constant()`     |
| 1–50% words changed                              | Delta `emit_delta_update(0x12, mask, payload)` |
| No words changed                                 | No emission (zero bandwidth)        |

#### Capacity Constraint

`MAX_PUSH_CONSTANT_WORDS = 32` (128 bytes / 4 — matches the Vulkan minimum guaranteed push constant size).

---

### DescriptorTracker

**File:** [synapse/descriptor_tracker.h](synapse/descriptor_tracker.h)  
**Namespace:** `synapse::replayer`  
**Responsibility:** Maintains a registry of all GPU resources and their current bindings per command buffer. Aggregates per-draw telemetry into `ResourceFootprintStats` for the `MetricsExporter`.

#### ResourceFootprintStats

| Field             | Description                                              |
|:------------------|:---------------------------------------------------------|
| `total_draws`     | Total draw/dispatch calls recorded                       |
| `sum_textures`    | Running sum of texture bindings per draw                 |
| `sum_buffers`     | Running sum of buffer bindings per draw                  |
| `sum_vram_bytes`  | Running sum of total VRAM bytes bound per draw           |
| `max_textures`    | Watermark — peak texture binding count in any single draw|
| `max_buffers`     | Watermark — peak buffer binding count                    |
| `max_vram_bytes`  | Watermark — peak VRAM footprint of any single draw       |

#### Metadata Record

Each resource in the registry exposes:
- `is_texture: bool` — used to separate texture vs. buffer bandwidth accounting
- `mip_levels: uint32_t` — total mip chain depth
- `resident_mips: uint32_t` — currently loaded mip levels (updated by ITS engine)
- `size_bytes: uint64_t` — total GPU allocation size

---

### SyncManager & FenceManager

**Files:** [synapse/sync_manager.h](synapse/sync_manager.h), [synapse/synapse_sync_manager.h](synapse/synapse_sync_manager.h)  
**Namespace:** `synapse::sync`, `synapse`  
**Responsibility:** `SyncManager` enforces the "Contract of Residency" — guaranteeing that no shader accesses a resource whose DMA transfer is still in-flight. `FenceManager` manages JIT compilation handshakes between the CPU Analyzer thread and the GPU timeline.

#### SyncManager Contract

```cpp
void mark_pending_load(uint64_t resource_id, uint64_t target_value);
void update_hardware_timeline(uint64_t current_value);  // call once per frame
bool is_safe_to_execute(uint64_t resource_id);           // must return true before shader access
```

- `hardware_timeline_value_` is atomic — written by hardware polling, read by draw call validation
- `is_safe_to_execute()` acquires `sync_mutex_` (coarse lock — upgrade to RW lock in production)
- Returns `true` if no pending DMA or if the hardware timeline has passed the required fence value

#### FenceManager Sequence IDs

$$ID_{completed} = \text{MMIO\_READ}(\text{SYNP\_TIMELINE\_REG})$$

`track_state_change(on_complete)` issues a monotonically increasing `SynapseSequenceID`. Callbacks are drained in `poll_gpu_timeline()` — called once per frame in the Analyzer loop.

---

### ConfidenceAggregator

**File:** [synapse/its_confidence_aggregator.h](synapse/its_confidence_aggregator.h)  
**Namespace:** `synapse::its`  
**Responsibility:** Produces a composite confidence score in [0.0, 1.0] from three independent sub-signals, used by the `DVFSController` to calibrate P-State requests.

#### Composite Score Formula

$$C = (V_{score} \times 0.4) + (M_{score} \times 0.4) + (H_{score} \times 0.2)$$

| Input Signal    | Weight | Source                                        |
|:----------------|:-------|:----------------------------------------------|
| Velocity score  | 0.4    | Rate of change in resource demand             |
| Mip gradient    | 0.4    | Direction and magnitude of mip streaming      |
| Historical score| 0.2    | Long-term frequency of this resource type     |

Output is clamped to [0.0, 1.0] via `std::clamp()`.

#### DVFS Threshold Constants

| Constant  | Value | Meaning                                            |
|:----------|:------|:---------------------------------------------------|
| `T_HIGH`  | 0.82  | Above this: request F0_MAX ramp-up                 |
| `T_LOW`   | 0.35  | Below this: allow P-State reduction / suppress boost|

---

### ForecastingProfiler

**File:** [synapse/forecasting_profiler.cpp](synapse/forecasting_profiler.cpp)  
**Namespace:** `synapse::its`  
**Responsibility:** Offline/background analysis tool that evaluates which temporal prediction horizon (5, 10, 20, or 30 frames) produces the best ratio of stalls avoided to false positives for a given workload signature.

#### Horizon Profiles

| Window | Best For                              | Tradeoff                          |
|:-------|:--------------------------------------|:----------------------------------|
| 5      | High-frequency resource cycling       | Low false positives               |
| 10     | Standard game rendering loops         | Balanced                          |
| 20     | Cutscenes, loading screens            | Higher waste on dynamic content   |
| 30     | Static SDF / raymarching scenes       | Maximum prefetch lead time        |

`HorizonStats.energy_efficiency` is computed as `stalls_avoided / (stalls_avoided + false_positives)` — reported to `report.json`.

---

### SmoothingEngine (PGRO)

**File:** [synapse/pgro_smoothing_engine.cpp](synapse/pgro_smoothing_engine.cpp)  
**Namespace:** `synapse` (implied)  
**Responsibility:** Profile-Guided Re-Optimization (PGRO) smooth engine. When a forecast confidence exceeds the `T_HIGH` threshold AND a significant shader complexity trend is detected, it proactively emits `SET_EXPECTED_LOAD` (Opcode `0x50`) HAI instructions to prime the scheduler for the upcoming compute spike.

#### Proactive Boost Logic

```
if (shader_trend > COMPLEXITY_THRESHOLD && forecast.confidence > 0.82f):
    emit HAI opcode 0x50 with estimated_cycles
    stats_.proactive_boosts++
```

The `suppress_boosts(bool)` API is called by `ThermalAwareArbiter` to prevent PGRO from requesting F0_MAX when thermal headroom is critically low.

---

### ThermalAwareArbiter

**File:** [synapse/thermal_aware_arbiter.cpp](synapse/thermal_aware_arbiter.cpp)  
**Namespace:** `synapse`  
**Responsibility:** The environmental hardening layer. When thermal headroom drops below 20%, it overrides the normal PGRO/DVFS flow by suppressing boosts, applying mip caps, and recording thermal mitigation events.

#### Thermal Mitigation Steps

1. `smoothing_engine_.suppress_boosts(true)` — block PGRO from requesting F0
2. `predictive_engine_.set_mip_cap(2)` — clamp texture streaming to mip level 2 (lower detail, less bandwidth)
3. Increment `stats_.thermal_mitigation_events`

When headroom recovers above 20%, all suppression and caps are cleared atomically.

---

## Data Flows

### Per-Frame Render Loop Data Flow

```
Frame N begins
  │
  ├─► SynapseCore::handle_draw_indexed()
  │     ├─ capture signature → push to TelemetryRingBuffer
  │     ├─ ITSEngine::prepare_for_use() → hysteresis check → async DMA if needed
  │     ├─ Scheduler::decide_backend() → read Analyzer recommendation
  │     └─ Execute: JIT | HAI | Oracle
  │
  ├─► [Background Thread] Analyzer::process_telemetry_loop()
  │     ├─ pop from ring buffer
  │     ├─ update workload model
  │     └─ update Scheduler recommendation
  │
  ├─► DVFSController::update_policy()
  │     ├─ ConfidenceAggregator::compute_composite_score()
  │     └─ initiate P-State transition if needed (75µs lock + 150nJ)
  │
  ├─► ThermalAwareArbiter::resolve_environmental_state()
  │     └─ suppress PGRO / cap mips if thermal_headroom < 0.20
  │
  ├─► SyncManager::update_hardware_timeline()
  │     └─ retire completed DMA fences
  │
  └─► FenceManager::poll_gpu_timeline()
        └─ execute JIT completion callbacks
```

---

## Telemetry Reference

All fields accumulate per-session and are exported to `report.json`.

| Module                 | Metric                      | Baseline        | Target    |
|:-----------------------|:----------------------------|:----------------|:----------|
| PredictiveEngine       | `accuracy_rate`             | 93.4%           | ≥ 90%     |
| ITSCacheController     | `hit_rate`                  | 89%             | ≥ 89%     |
| ITSCacheController     | `sync_stalls`               | 12              | ≤ 15      |
| PowerEstimator         | `avg_milliwatts_saved`      | TBD             | > 200 mW  |
| FenceManager           | `stability_overrides_count` | TBD             | < 5/run   |
| ThermalAwareArbiter    | `thermal_mitigation_events` | TBD             | < 3/run   |
| SmoothingEngine        | `proactive_boosts`          | TBD             | measured  |
| PushConstantOptimizer  | `get_change_rate()`         | TBD             | < 0.4     |

---

## Edge Cases & Fallback Behaviors

### 1. JIT Cold Cache (First Frame)

- **Condition:** `JITSpecializationCache::get(hash)` returns `nullptr` — no compiled ISA exists
- **Detection:** `JITPipeline::get_optimized_shader()` returns `nullptr`
- **Response:** `SynapseCore::execute_jit_path()` falls back to `orig_draw_indexed_()` (Oracle)
- **Recovery:** Analyzer compilation completes in background; cache populated for frame N+1

### 2. Thermal Headroom < 20% (Mip-Cap Event)

- **Condition:** `thermal_headroom < 0.20f` in `ThermalAwareArbiter`
- **Detection:** PMU thermal register read, polled every frame
- **Response:** PGRO boosts suppressed, `mip_cap = 2` applied to `PredictiveEngine`
- **Cost:** Lower texture detail, but frame rate maintained. Recorded in `thermal_mitigation_events`
- **Recovery:** Automatic on headroom recovery above 20%

### 3. DVFS Hysteresis Conflict (Within 5-Frame Window)

- **Condition:** A P-State request arrives while `frames_since_switch_ ≤ 5`
- **Detection:** Standard `if (requested != current_state_ && frames_since_switch_ > 5)` guard
- **Response:** Request silently dropped. If workload is genuinely heavy, `handle_sync_stall()` emergency override bypasses hysteresis
- **Risk:** At 60 FPS, the 5-frame window is 83 ms — acceptable oscillation prevention for non-emergency transitions

### 4. DMA Fence Timeout (Sync Stall)

- **Condition:** `SyncManager::is_safe_to_execute()` returns `false` — the DMA for a prefetched texture has not completed by the time the shader needs it
- **Detection:** `SyncManager` timeline comparison; counted in `cache_metrics.sync_stalls`
- **Response:** `TextureStreamingEngineHardened::get_safe_mip_level()` returns the next lower resident mip; shader continues without stall using reduced-detail texture
- **Baseline:** 12 sync stalls recorded in `report.json` — acceptable for MVP

### 5. Analyzer Prediction Failure → Oracle Fallback

- **Condition:** Scheduler recommendation is stale, or Analyzer hasn't converged
- **Detection:** Backend returns `ExecutionBackend::Oracle`, or `JITPipeline` returns `nullptr`
- **Response:** Native `orig_draw_indexed_()` called directly — zero Synapse overhead
- **Cost:** Frame rendered at baseline (no optimization). Not counted as a failure.
- **Recovery:** Automatic — on next frame after Analyzer update

---

## Canonical Data Structures

These types are the single source of truth across all modules.

```cpp
// synapse/synapse_umd.h
struct WorkloadSignature {
    uint32_t draw_call_count;
    uint32_t pipeline_state_changes;
    uint32_t shader_instruction_estimate;
    uint32_t vertex_count;
    uint32_t texture_bindings;
    bool     is_compute_dispatch;
};

enum class ExecutionBackend { JIT, HAI, Oracle };

// synapse/dvfs_controller.h
enum class PState { F0_MAX, F1_BALANCED, F2_EFFICIENT };

// synapse/sync_manager.h
struct ResourceSyncState {
    uint64_t pending_fence_value;
    bool     is_dirty;
};

// synapse/hai_frontend_sim.h
struct SimulationMetrics {
    uint64_t total_cycles;
    uint64_t total_bytes_fetched;
    uint32_t delta_updates_processed;
    uint32_t cache_hits;
    uint32_t cache_misses;
};

// synapse/its_engine_hardened.h
struct MipResidencyState {
    std::atomic<bool>     is_resident;
    std::atomic<uint64_t> dma_fence_id;
    float                 current_priority;
};
```

---

## API Contracts

### SynapseCore (Entry Point)

| Method                  | Thread     | Precondition                    | Postcondition                            |
|:------------------------|:-----------|:--------------------------------|:-----------------------------------------|
| `handle_draw_indexed()` | Render     | `orig_draw_indexed_` != nullptr | Draw submitted via optimal backend       |
| `~SynapseCore()`        | Any        | Analyzer running                | Analyzer thread joined, resources freed  |

### ITSCacheController (Resource Safety)

| Method               | Thread      | Precondition            | Returns                        |
|:---------------------|:------------|:------------------------|:-------------------------------|
| `access_resource()`  | Any (locked)| Valid `resource_id`     | `true` = resident, `false` = miss |
| `remove_resource()`  | Any (locked)| Resource previously tracked | Usage decremented, LRU pruned |

### SyncManager (Execution Safety)

| Method                    | Thread      | Returns                               |
|:--------------------------|:------------|:--------------------------------------|
| `is_safe_to_execute(id)`  | Render      | `true` only if no in-flight DMA       |
| `mark_pending_load(id, v)`| Prefetch    | Records fence; marks resource dirty   |
| `update_hardware_timeline(v)` | Poll    | Atomically updates hardware progress  |

---

## Scope Limits

Project Synapse explicitly does NOT:

1. **Replace the kernel-mode driver (KMD).** Synapse operates entirely in user space within the UMD. Kernel scheduler decisions, PCIe/memory controller arbitration, and interrupt handling are outside its scope.
2. **Modify GPU firmware or microcode.** All co-design hardware suggestions (HAI RISC-V frontend, on-die ML inference core) are proposals for future hardware revisions, not current functionality.
3. **Provide OpenGL / Vulkan 1.0 legacy path optimization.** Synapse hooks `vkCmdDrawIndexed`, `vkCmdDraw`, `vkCmdDispatch`, `vkCmdPushConstants`, `vkCmdBindDescriptorSets`, `vkCreateImage`/`vkDestroyImage` in the Vulkan 1.3+ command stream. Legacy API translation layers (DXVK, VKD3D) feed into Synapse naturally but are not Synapse's responsibility.
4. **Make game-engine-level LOD decisions.** Mip-cap during thermal mitigation is a hardware-level residency control, not a semantic LOD choice. The game engine's LOD system remains authoritative.
5. **Guarantee performance on discrete GPUs.** Synapse is tuned for the shared-memory, bandwidth-constrained architecture of integrated graphics. Discrete GPU behavior is untested and unsupported.
