# Project Synapse — Getting Started Guide

**For:** New engineers joining the iGPU_Shim project  
**Prerequisite knowledge:** C++20, Vulkan command recording, basic GPU driver concepts

---

## 1. What Is Project Synapse?

Project Synapse is a **User-Mode Driver (UMD) shim** that sits between a Vulkan/D3D12 application and the real iGPU driver. It intercepts `vkCmdDrawIndexed` calls and dynamically routes them to one of three execution backends:

| Backend | When Used | Why |
|---------|-----------|-----|
| **JIT** | Complex vertex/fragment shaders | Specialises ISA to current workload — reduces register pressure |
| **HAI** | High-frequency simple draws | Delta-compresses the command stream — reduces bandwidth by ≥ 4× |
| **Oracle** | Unknown workloads, first-frame cold cache | Passes through unchanged — zero regression guarantee |

The shim also governs iGPU P-State proactively (DVFS), prefetches textures ahead of demand (ITS), and emits energy telemetry to `report.json`.

---

## 2. Repository Layout

```
iGPU_Shim/
├── synapse/                  # All C++ source code
│   ├── synapse_core.h/.cpp   # Central coordinator — start reading here
│   ├── synapse_umd.h         # Wire types: WorkloadSignature, TelemetryRingBuffer
│   ├── hash_utils.h          # Canonical hash functions (DRY — do not re-implement)
│   ├── telemetry_types.h     # All report structs (DRY — do not re-implement)
│   ├── platform_config.h     # Per-SKU constants: pj_per_bit, thermal thresholds
│   ├── dvfs_controller.h/.cpp# Frequency/voltage governance
│   ├── sync_manager.h        # DMA fence safety — "Contract of Residency"
│   ├── power_estimator.h     # Energy impact model
│   ├── jit_specialization_cache.h  # Lock-free JIT cache with collision detection
│   ├── synapse_hai_builder.h # HAI bytecode builder with compression counters
│   ├── forecasting_profiler.cpp    # Horizon analysis (5/10/20/30-frame windows)
│   ├── its_engine_hardened.h       # Texture Streaming Engine
│   ├── its_cache_controller.h      # LRU cache controller
│   ├── predictive_engine.h         # Temporal locality predictor
│   ├── its_confidence_aggregator.h # Composite confidence score [0,1]
│   ├── thermal_aware_arbiter.cpp   # Thermal mitigation + PGRO gating
│   ├── pgro_smoothing_engine.cpp   # Proactive clock domain pre-warm
│   ├── synapse_jit_backend.h       # JIT pipeline + specialised ISA struct
│   ├── hai_frontend_sim.h          # HAI bytecode cycle simulator
│   ├── push_constant_optimizer.h   # Push constant delta emission
│   ├── descriptor_tracker.h        # Draw call state tracking + telemetry
│   ├── tests/
│   │   ├── test_ring_buffer.cpp    # TelemetryRingBuffer concurrency tests
│   │   └── test_edge_cases.cpp     # Five "What if?" smoke tests
│   └── tools/
│       ├── gen_panning_trace.py    # Generates synthetic 1000-frame ITS traces
│       └── bench_critical_path.cpp # CPU cycle benchmark for handle_draw_indexed
├── docs/
│   └── getting_started.md    # ← You are here
├── report.json               # Telemetry baseline + schema v2.0.0
├── documentation.md          # Full technical reference
├── plan.md                   # Engineering roadmap (phases 0–7)
└── README.md                 # Project overview and architecture diagram
```

---

## 3. Prerequisites

### Compiler
- GCC 12+ or Clang 16+ with full C++20 support
- `std::shared_mutex`, `std::atomic<std::shared_ptr<T>>`, `std::hardware_destructive_interference_size`

### Optional Tools
- **Thread Sanitizer (TSan):** `-fsanitize=thread` — required for concurrency test runs
- **Doxygen 1.9+:** To generate HTML API reference from `///` comments
- **Python 3.10+:** To run `tools/gen_panning_trace.py`

### Environment Variables

| Variable | Effect |
|----------|--------|
| `SYNAPSE_PLATFORM_OVERRIDE=LPDDR4X` | Override memory type for energy model (also: `DDR5`) |
| `SYNAPSE_POWER_VERIFY` (compile flag) | Enable PowerEstimator assertions in CI / lab builds |

---

## 4. Building the Tests

All tests live in `synapse/tests/`. They are standalone compilation units that `#include` relative paths from `synapse/`.

### Ring Buffer Concurrency Test
```bash
cd synapse/tests
g++ -std=c++20 -fsanitize=thread -I.. test_ring_buffer.cpp -o test_ring_buffer
./test_ring_buffer
```
Expected output: 5 `PASS` lines. Zero TSan warnings.

### Edge-Case Smoke Tests
```bash
cd synapse/tests
g++ -std=c++20 -I.. test_edge_cases.cpp -o test_edge_cases
./test_edge_cases
```
Expected output: 5 `PASS` lines covering oracle fallback, JIT cold cache, sync stall, DVFS hysteresis, and thermal mitigation.

### CPU Cycle Benchmark
```bash
cd synapse/tools
g++ -std=c++20 -O2 -I.. bench_critical_path.cpp -o bench_critical_path
./bench_critical_path 10000
```
Acceptance criteria checked automatically: p99 ≤ 1 µs, overhead < 20%. Exit code 0 = all pass.

---

## 5. Code Reading Order for New Engineers

1. **`synapse_umd.h`** — Read the `WorkloadSignature` struct and `TelemetryRingBuffer`. These are the two foundational data structures everything else is built around.

2. **`synapse_core.h`** — Read `SynapseCore::handle_draw_indexed()`. This is the single entry point into all Synapse logic.

3. **`hash_utils.h`** — Understand the context hash. Every cache lookup flows through `util::workload_context_hash()`.

4. **`dvfs_controller.h`** — Read the method signatures and Doxygen. You do not need to read `.cpp` first.

5. **`sync_manager.h`** — Read `SyncManager::is_safe_to_execute()` to understand the "Contract of Residency" (the guarantee that shaders never access a resource with an in-flight DMA).

6. **`tests/test_edge_cases.cpp`** — Read the five tests. They are the executable specification of the five "What if?" scenarios from plan.md Phase 0.

---

## 6. Key Invariants — Never Break These

| Invariant | Where enforced |
|-----------|----------------|
| `push()` on `TelemetryRingBuffer` NEVER blocks the render thread | `synapse_umd.h` — returns `false` on overflow |
| A shader NEVER accesses a resource with `is_dirty = true` | `sync_manager.h` — `is_safe_to_execute()` |
| Hash functions are ONLY implemented in `hash_utils.h` | DRY rule — do not add `hash_combine` elsewhere |
| Telemetry structs are ONLY defined in `telemetry_types.h` | DRY rule — do not re-declare `HorizonStats` etc. |
| Per-SKU constants come ONLY from `PlatformConfig::get()` | No magic numbers for `pj_per_bit` or `thermal_mitigation_threshold` |
| `DVFSController::handle_sync_stall()` is the ONLY hysteresis bypass | Only called from `SynapseCore::resolve_power_perf_conflict()` |

---

## 7. Adding a New Module

1. Create `synapse/your_module.h` with `#pragma once` and a namespace under `synapse::`.
2. If it emits telemetry, add a stats struct to **`telemetry_types.h`** (do not define it in your own header).
3. If it uses platform constants, read from **`PlatformConfig::get()`**.
4. If it uses hash functions, include **`hash_utils.h`**.
5. Add a test in `synapse/tests/` that covers at least the happy path and one "What if?" failure mode.
6. Add the new file to the Phase 6 Deliverables table in `plan.md`.

---

## 8. Generating the API Reference

```bash
# From the workspace root
doxygen -g Doxyfile          # Only needed once; edit INPUT = synapse/
doxygen Doxyfile
open html/index.html
```

Every public method in the codebase has a `@brief` Doxygen comment. Parameter and return documentation is on all non-trivial functions.

---

## 9. Reporting & Telemetry

At session end, `SynapseSessionReport` (defined in `telemetry_types.h`) is serialised to `report.json`. The schema is documented in `report.json` itself (v2.0.0).

To populate the horizon_analysis section, call:
```cpp
forecasting_profiler_.serialize_to_report(session_report);
```

To validate the power model in CI, build with `-DSYNAPSE_POWER_VERIFY` and call:
```cpp
power_estimator_.verify();
```

---

*For the full technical reference, see [documentation.md](../documentation.md).  
For the phased engineering roadmap, see [plan.md](../plan.md).*
