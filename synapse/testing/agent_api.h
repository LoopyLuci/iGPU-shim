#pragma once

// Include the full SynapseCore so AgentAPI::snapshot() can query live subsystems.
// This is acceptable in a testing/inspection header (not a hot-path include).
#include "../synapse_core.h"
#include "../telemetry_types.h"
#include "../synapse_umd.h"
#include "../dvfs_controller.h"   // synapse::power::PState for SynapseInjector
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
    // Capture a snapshot.
    // Pass a non-null `core` pointer to populate the report from live subsystems
    // (JIT stutter stats, ITS counters, ML bandit stats, power report).
    // T3-5: wires snapshot to real SynapseCore state.
    SystemSnapshot snapshot(synapse::SynapseCore* core = nullptr) const {
        SystemSnapshot s{};
        if (core) {
            s.report = core->build_session_report();
        }
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

    void set_dvfs_state(synapse::power::PState p) {
        (void)p;
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
    SynapseInjector  injector;

    // Convenience overload: snapshot without a live core (returns zeroed report).
    SystemSnapshot snapshot() { return inspector.snapshot(nullptr); }

    // Live snapshot: pulls data from the running SynapseCore instance.
    SystemSnapshot snapshot(synapse::SynapseCore* core) { return inspector.snapshot(core); }
};

} // namespace synapse::testing
