// ============================================================================
// synapse/synapse_sync_manager.h
// Project Synapse – Timeline Synchronization & Barrier Orchestrator
// ============================================================================
#pragma once

#include "synapse_umd.h"
#include <atomic>
#include <queue>

namespace synapse {

using SynapseSequenceID = uint64_t;

struct PendingStateChange {
    SynapseSequenceID id;
    std::function<void()> action;
    bool is_complete = false;
};

/**
 * @class FenceManager
 * @brief Manages the handshake between the CPU-side Analyzer and the GPU Timeline.
 */
class FenceManager {
public:
    FenceManager() : last_completed_id_(0), next_id_(1) {}

    // Generates a new ID for a state change (e.g., a JIT compilation finish)
    SynapseSequenceID track_state_change(std::function<void()> on_complete) {
        SynapseSequenceID id = next_id_.fetch_add(1, std::memory_order_relaxed);
        pending_changes_.push({id, on_complete});
        return id;
    }

    // Called once per frame to retire completed GPU work
    void poll_gpu_timeline() {
        // Query the actual hardware register for the current completed ID
        // $$ ID_{completed} = \text{MMIO\_READ}(SYNP\_TIMELINE\_REG) $$
        uint64_t gpu_val = hardware_read_timeline_id();
        last_completed_id_.store(gpu_val, std::memory_order_release);

        // Execute callbacks for all changes the GPU has now passed
        while (!pending_changes_.empty() && pending_changes_.front().id <= gpu_val) {
            pending_changes_.front().action();
            pending_changes_.pop();
        }
    }

    // Defensive Check: Is it safe to use this resource/shader?
    bool is_safe_to_execute(SynapseSequenceID required_id) const {
        return last_completed_id_.load(std::memory_order_acquire) >= required_id;
    }

private:
    uint64_t hardware_read_timeline_id() {
        // Platform-specific: Interrogate the iGPU Command Processor
        return 0; 
    }

    std::atomic<uint64_t> last_completed_id_;
    std::atomic<uint64_t> next_id_;
    std::queue<PendingStateChange> pending_changes_;
};

} // namespace synapse