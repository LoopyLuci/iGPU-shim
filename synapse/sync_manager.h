// ============================================================================
// synapse/sync_manager.h
// Project Synapse – Timeline Synchronization & Fence Management
// ============================================================================
#pragma once

#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <atomic>

namespace synapse::sync {

/**
 * @struct ResourceSyncState
 * @brief Tracks the synchronization status of a specific GPU resource.
 */
struct ResourceSyncState {
    uint64_t pending_fence_value = 0; // The timeline value required for safety
    bool is_dirty = false;           // True if a DMA operation is in-flight
};

/**
 * @class SyncManager
 * @brief Manages the "Contract of Residency" between DMA and Execution.
 */
class SyncManager {
public:
    SyncManager() : hardware_timeline_value_(0) {}

    /**
     * @brief Records a pending DMA operation for a resource.
     * @param resource_id The handle of the texture/buffer.
     * @param target_value The timeline value the DMA engine will signal on completion.
     */
    void mark_pending_load(uint64_t resource_id, uint64_t target_value) {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        resource_states_[resource_id] = { target_value, true };
    }

    /**
     * @brief Updates the manager with the current progress of the hardware.
     * In trace mode, this is simulated; in production, this reads a GPU register.
     */
    void update_hardware_timeline(uint64_t current_value) {
        hardware_timeline_value_.store(current_value, std::memory_order_release);
    }

    /**
     * @brief Validates if a resource is safe for Shader access.
     * @return True if no DMA is pending or if the pending DMA has completed.
     */
    bool is_safe_to_execute(uint64_t resource_id) {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        auto it = resource_states_.find(resource_id);
        
        if (it == resource_states_.end() || !it->second.is_dirty) {
            return true; // No pending operations
        }

        // Compare against the atomic hardware progress
        if (hardware_timeline_value_.load(std::memory_order_acquire) >= it->second.pending_fence_value) {
            it->second.is_dirty = false; // Operation completed, clear the dirty flag
            return true;
        }

        return false; // DMA still in progress
    }

private:
    std::mutex sync_mutex_;
    std::atomic<uint64_t> hardware_timeline_value_;
    std::unordered_map<uint64_t, ResourceSyncState> resource_states_;
};

} // namespace synapse::sync