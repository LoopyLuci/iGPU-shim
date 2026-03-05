// ============================================================================
// synapse/sync_manager.h
// Project Synapse – Timeline Synchronization & Fence Management
// ============================================================================
#pragma once

#include <unordered_map>
#include <shared_mutex>   // std::shared_mutex, std::shared_lock, std::unique_lock
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
     * @note Takes an exclusive (write) lock — only the prefetch path calls this.
     */
    void mark_pending_load(uint64_t resource_id, uint64_t target_value) {
        std::unique_lock<std::shared_mutex> lock(sync_mutex_);
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
     * @note Hot path: takes a shared (read) lock first. Upgrades to exclusive only
     *       when clearing the dirty flag, avoiding contention on the render thread.
     */
    bool is_safe_to_execute(uint64_t resource_id) {
        // --- Fast path: shared (read) lock ---
        {
            std::shared_lock<std::shared_mutex> read_lock(sync_mutex_);
            auto it = resource_states_.find(resource_id);

            if (it == resource_states_.end() || !it->second.is_dirty) {
                return true; // No pending operations
            }

            // DMA still in-flight if timeline hasn't reached the fence yet
            if (hardware_timeline_value_.load(std::memory_order_acquire)
                    < it->second.pending_fence_value) {
                return false;
            }
        }
        // --- Slow path: exclusive (write) lock to clear dirty flag ---
        {
            std::unique_lock<std::shared_mutex> write_lock(sync_mutex_);
            auto it = resource_states_.find(resource_id);
            // Re-check under write lock (another thread may have cleared it first)
            if (it != resource_states_.end() && it->second.is_dirty
                    && hardware_timeline_value_.load(std::memory_order_acquire)
                           >= it->second.pending_fence_value) {
                it->second.is_dirty = false;
            }
        }
        return true;
    }

    /**
     * @brief Registers a global bus lock — all draw calls must stall until
     *        the DVFS transition window has elapsed.
     * @param unlock_at_us Absolute microsecond timestamp after which the lock lifts.
     */
    void register_global_bus_lock(uint64_t unlock_at_us) {
        std::unique_lock<std::shared_mutex> lock(sync_mutex_);
        global_bus_lock_until_us_ = unlock_at_us;
    }

    /**
     * @brief Returns true if the global DVFS bus lock has elapsed.
     * @param current_us Current timestamp in microseconds.
     */
    bool is_bus_lock_clear(uint64_t current_us) const {
        std::shared_lock<std::shared_mutex> lock(sync_mutex_);
        return current_us >= global_bus_lock_until_us_;
    }

private:
    mutable std::shared_mutex sync_mutex_;          // mutable: const query methods need shared lock
    std::atomic<uint64_t>     hardware_timeline_value_;
    uint64_t                  global_bus_lock_until_us_ = 0;
    std::unordered_map<uint64_t, ResourceSyncState> resource_states_;
};

} // namespace synapse::sync