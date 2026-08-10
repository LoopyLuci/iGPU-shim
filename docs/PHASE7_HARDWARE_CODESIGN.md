# Phase 7 — Hardware Co-Design Proposal & iGPU Testing Strategy

**Date:** August 8, 2026
**Status:** ACTIVE — Hardware Available
**Target:** Intel UHD Graphics (Comet Lake, Device ID 9B41) on Windows 10

---

## Executive Summary

The target hardware is **available and functional**. This document outlines the
co-design strategy for integrating Synapse with the real Intel iGPU, replacing
stub implementations with production hardware paths, and establishing a repeatable
testing methodology.

---

## 1. Hardware Inventory

| Property | Value |
|----------|-------|
| **GPU** | Intel UHD Graphics (Comet Lake) |
| **Device ID** | `0x9B41` (VEN_8086) |
| **Driver** | Intel proprietary, version 27.20.100.9365 |
| **Vulkan API** | 1.2.170 |
| **Conformance** | 1.2.3.2 |
| **Memory** | 1 GB shared (LPDDR5, system memory) |
| **Type** | Integrated GPU (PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) |

### Key Characteristics

- **Shared memory architecture**: iGPU shares system RAM — bandwidth is the
  primary bottleneck, not VRAM capacity
- **Power-constrained**: Thermal design power (TDP) sharing with CPU means
  power budget is a hard ceiling
- **DVFS available**: Intel GPU supports frequency scaling (render/media rings)
- **No dedicated VRM**: Power delivery is shared with CPU package

---

## 2. What Can Be Tested Without Kernel Access

Most of Synapse's production modules are **user-space only** and can be tested
today with the real iGPU:

### 2.1 Vulkan Implicit Layer Loading

The layer manifest (`VkLayer_synapse.json`) can be deployed and tested:

```powershell
# Set Vulkan layer path
$env:VK_LAYER_PATH = "C:\Users\limpi\iGPU_Shim\build_stub\Release"

# Test layer loading with vulkaninfo
vulkaninfo --summary
```

**Test**: Does `SynapseLayer.dll` load without crashing when a Vulkan app starts?

### 2.2 Draw Call Interception

Synapse intercepts `vkCmdDrawIndexed`. With the real iGPU:

1. Create a minimal Vulkan app that draws a triangle
2. Enable the Synapse implicit layer
3. Verify interceptors fire (WAL entries appear)
4. Measure latency overhead vs. native Vulkan calls

**Test**: Does the shim add measurable overhead to actual draw calls?

### 2.3 Power Budget Validation

The Intel iGPU exposes power via `VK_EXT_physical_device_drm` or Windows
`D3DKMTQueryAdapterInfo`. Synapse's power governance can be validated:

- Read real GPU power consumption
- Compare against `AtomicConfig.power_budget_watts` (default 15W)
- Verify thermal mitigation triggers at real temperatures

**Test**: Does the power budget model match real hardware behavior?

### 2.4 Memory Bandwidth Measurement

The iGPU shares system memory. Synapse's memory subsystem can be tested:

- Real texture upload/download throughput
- Cache hit rates with real workload patterns
- Bandwidth saturation behavior under load

**Test**: Do the telemetry numbers match real hardware capabilities?

---

## 3. What Requires Kernel Access (and How to Work Around It)

### 3.1 Fence Queries (`SYNAPSE_REAL_FENCE`)

**Original blocker**: `D3DKMTWaitForSynchronizationObject2` requires kernel-mode
access via `d3dkmthk.h`.

**Workaround**: Use the **Vulkan fence API** instead of D3DKMT:

```cpp
// Instead of D3DKMT fence:
vkWaitForFences(device, 1, &fence, VK_TRUE, timeout_ns);
vkGetFenceStatus(device, fence, &status);
```

Vulkan fences are user-space accessible and provide the same synchronization
semantics. The `SYNAPSE_REAL_FENCE` flag can be mapped to Vulkan fence queries
on Windows.

**Status**: ✅ Implementable without kernel access.

### 3.2 DMA Transfers (`SYNAPSE_REAL_DMA`)

**Original blocker**: KMD DMA via `drmIoctl` (Linux) or `D3DKMTRender` (Windows).

**Workaround**: Use **Vulkan buffer copy commands**:

```cpp
// Instead of KMD DMA:
vkCmdCopyBuffer(cmdBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
```

Vulkan buffer copies go through the GPU's built-in DMA engine. This gives us
real DMA throughput without kernel access.

**Status**: ✅ Implementable without kernel access.

### 3.3 DVFS Control

**Original blocker**: Direct frequency scaling requires kernel driver access.

**Workaround**: Use **Vulkan extension queries** to read current GPU frequency,
and use **power capping** via Windows `PowerCreateRequest`:

```cpp
// Read GPU frequency (if VK_EXT_physical_device_drm is available)
VkPhysicalDeviceDrmPropertiesEXT drmProps{};
// ... query chain ...

// Or use Intel-specific extension
// VK_INTEL_performance_query for real GPU metrics
```

**Status**: 🟡 Read-only frequency available. Write control requires driver API.

### 3.4 Telemetry Hardware Counters

**Original blocker**: GPU performance counters require vendor-specific APIs.

**Workaround**: Use **Vulkan timestamp queries**:

```cpp
vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
// ... draw call ...
vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
```

This gives GPU execution time without vendor-specific APIs.

**Status**: ✅ Implementable without kernel access.

---

## 4. Implementation Plan

### Stage 1: Layer Loading Test (1 hour)

1. Build `SynapseLayer.dll` with `SYNAPSE_STUB_DMA=ON`
2. Deploy `VkLayer_synapse.json` to Vulkan layer path
3. Run `vulkaninfo` with layer enabled
4. Verify WAL file is created and entries appear

**Deliverable**: `test_layer_load.cpp` — verifies implicit layer loading.

### Stage 2: Draw Call Interception (2-3 hours)

1. Create minimal Vulkan triangle app (`test_triangle.cpp`)
2. Enable Synapse layer
3. Verify `handle_draw_indexed` fires with real GPU
4. Measure overhead: native vs. shimmed draw calls
5. Compare with benchmark numbers (should match ~4μs)

**Deliverable**: `test_draw_intercept.cpp` — real GPU draw call interception.

### Stage 3: Power & Thermal Validation (2-3 hours)

1. Read real GPU power via Intel extension or Windows API
2. Compare with `AtomicConfig.power_budget_watts`
3. Run sustained workload, monitor thermal behavior
4. Verify `GracefulDegradation` triggers correctly

**Deliverable**: `test_power_validation.cpp` — real hardware power measurements.

### Stage 4: Memory Subsystem (1-2 hours)

1. Allocate real Vulkan buffers on iGPU
2. Measure upload/download throughput
3. Compare with `report.json` baseline (68,000 MB/s theoretical)
4. Validate cache behavior with real textures

**Deliverable**: `test_memory_subsystem.cpp` — real bandwidth measurements.

---

## 5. Test App: Minimal Vulkan Triangle

```cpp
// test_triangle.cpp — Minimal Vulkan app for Synapse layer testing
// Creates a window, renders a triangle, measures frame time.

#include <vulkan/vulkan.h>
#include <windows.h>
#include <cstdio>
#include <chrono>
#include <vector>

// Key steps:
// 1. Create VkInstance with layer enabled
// 2. Select Intel UHD Graphics (physical device)
// 3. Create logical device + command buffer
// 4. Render triangle in a loop
// 5. Measure frame time (native vs. shimmed)
// 6. Report results

int main() {
    // Check if Synapse layer is loaded
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());

    bool synapseLoaded = false;
    for (auto& l : layers) {
        if (strstr(l.layerName, "synapse") || strstr(l.layerName, "Synapse")) {
            synapseLoaded = true;
            printf("Synapse layer found: %s\n", l.layerName);
        }
    }
    if (!synapseLoaded) {
        printf("WARNING: Synapse layer not found in Vulkan layers.\n");
        printf("Set VK_LAYER_PATH to include SynapseLayer.dll directory.\n");
    }

    // ... rest of Vulkan setup and rendering loop ...
    return 0;
}
```

---

## 6. Success Criteria

| Test | Criterion | Priority |
|------|-----------|----------|
| Layer loading | `SynapseLayer.dll` loads without crash | P0 |
| Draw interception | WAL entries appear for real draw calls | P0 |
| Overhead measurement | < 10μs per draw call (matches benchmark) | P0 |
| Power reading | Real GPU power readable via Vulkan/API | P1 |
| Memory bandwidth | Real throughput matches expectations | P1 |
| Thermal monitoring | Temperature readable and thresholds work | P1 |
| Graceful degradation | Features disable correctly under load | P2 |

---

## 7. Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Layer fails to load | Check Vulkan SDK version, layer manifest path |
| iGPU not detected | Verify `VK_LAYER_PATH`, check `vulkaninfo` output |
| Draw calls not intercepted | Verify `vkGetDeviceProcAddr` hooks, check WAL |
| Overhead too high | Profile hot path, optimize WAL batch size |
| Driver crashes | Use validation layers (`VK_LAYER_KHRONOS_validation`) |

---

## 8. Files to Create

```
synapse/tools/test_layer_load.cpp      — Layer loading verification
synapse/tools/test_draw_intercept.cpp  — Real GPU draw call test
synapse/tools/test_power_validation.cpp — Hardware power measurement
synapse/tools/test_memory_subsystem.cpp — Real bandwidth measurement
```

All tools are standalone executables that run against the real iGPU.
No kernel access required. No Docker. 100% native.

---

## 9. Next Actions

1. **TODAY**: Build and deploy layer, verify loading with `vulkaninfo`
2. **THIS WEEK**: Create `test_triangle.cpp`, measure real draw call overhead
3. **NEXT WEEK**: Power validation and memory subsystem tests
4. **ONGOING**: Compare real hardware numbers against `report.json` baseline

---

*Phase 7 bridges the gap between synthetic benchmarks and real hardware validation.
All tests run on the actual Intel UHD Graphics iGPU with zero kernel dependencies.*
