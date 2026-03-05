#pragma once

#include "telemetry_types.h"
#include "synapse_umd.h"
#include <vector>
#include <optional>
#include <string>

namespace synapse::testing {

// Snapshot of system state for agents/tests. Contains high-level telemetry.
struct SystemSnapshot {
    telemetry::SynapseSessionReport report;
    uint64_t frame_counter = 0;
    // Extendable: runtime flags, active PState, thermal headroom
};

// Lightweight inspector to capture a serialisable snapshot of the running system.
class SynapseInspector {
public:
    // Capture a snapshot. Implementations in the UMD must populate fields.
    SystemSnapshot snapshot() const {
        SystemSnapshot s{};
        // In production this calls into SynapseCore to fill `report`.
        return s;
    }
};

// Injector API: allows deterministic scenario replay by injecting synthetic state.
class SynapseInjector {
public:
    // Inject a synthetic workload into the pipeline (bypasses real draw call).
    void inject_workload(const WorkloadSignature& sig) {
        // In production: push sig into TelemetryRingBuffer and wake Analyzer.
    }

    void set_thermal_headroom(float headroom) {
        // In production: write into PlatformConfig override or Thermal subsystem
    }

    void set_dvfs_state(PState p) {
        // Force DVFSController to the requested PState (testing only)
    }

    void force_cache_miss(uint64_t shader_hash) {
        // Evict shader from JITSpecializationCache
    }

    void inject_dma_stall(uint64_t resource_id) {
        // Mark resource as dirty in SyncManager
    }

    void set_frame_counter(uint64_t f) {
        // Advance internal frame counter
    }
};

// Agent-facing master API combining inspector + injector
class AgentAPI {
public:
    SynapseInspector inspector;
    SynapseInjector injector;

    SystemSnapshot snapshot() { return inspector.snapshot(); }
};

} // namespace synapse::testing
