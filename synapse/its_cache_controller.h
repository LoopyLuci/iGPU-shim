// ============================================================================
// synapse/its_cache_controller.h
// Project Synapse – Unified Synaptic Cache with LRU Eviction (Phase 3 MVP)
// ============================================================================
#pragma once

#include <list>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <functional>
#include <iostream>

#include "descriptor_tracker.h"

namespace synapse::its {

/**
 * @class ITSCacheController
 * @brief Manages the iGPU Synaptic Cache capacity and residency using an LRU policy.
 * Thread-safe for integration with asynchronous pre-fetch queues.
 */
class ITSCacheController {
public:
    /**
     * @brief Constructs the cache controller with a strict byte limit.
     * @param capacity_bytes The maximum allowed residency footprint.
     * @param size_lookup_cb Callback to fetch resource sizes from the Registry.
     */
    ITSCacheController(uint64_t capacity_bytes, std::function<uint64_t(uint64_t)> size_lookup_cb) 
        : capacity_(capacity_bytes), 
          current_usage_(0), 
          hits_(0), 
          misses_(0),
          get_resource_size_callback_(std::move(size_lookup_cb)) {}

    /**
     * @brief Requests access to a resource, updating its LRU status or loading it.
     * @param resource_id The Vulkan handle of the resource.
     * @param required_bytes The size of the specific mip-range requested.
     * @return True if the resource was already resident (Hit), False otherwise (Miss).
     */
    bool access_resource(uint64_t resource_id, uint64_t required_bytes) {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        auto it = lru_map_.find(resource_id);
        if (it != lru_map_.end()) {
            // Cache Hit: Move to the front (Most Recently Used)
            lru_list_.erase(it->second);
            lru_list_.push_front(resource_id);
            lru_map_[resource_id] = lru_list_.begin();
            hits_++;
            return true;
        }

        // Cache Miss: Evict if necessary, then insert
        misses_++;
        ensure_capacity_locked(required_bytes);
        
        lru_list_.push_front(resource_id);
        lru_map_[resource_id] = lru_list_.begin();
        current_usage_ += required_bytes;
        
        return false;
    }

    /**
     * @brief Safely removes a resource from the cache (e.g., during vkDestroyImage).
     * @param resource_id The Vulkan handle to remove.
     */
    void remove_resource(uint64_t resource_id) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = lru_map_.find(resource_id);
        if (it != lru_map_.end()) {
            uint64_t freed_bytes = get_resource_size_callback_(resource_id);
            current_usage_ -= freed_bytes;
            lru_list_.erase(it->second);
            lru_map_.erase(it);
        }
    }

    // Telemetry API
    uint64_t get_hits() const { return hits_; }
    uint64_t get_misses() const { return misses_; }
    uint64_t get_current_usage() const { return current_usage_; }
    float get_hit_rate() const { 
        uint64_t total = hits_ + misses_;
        return total > 0 ? static_cast<float>(hits_) / static_cast<float>(total) : 0.0f; 
    }

private:
    /**
     * @brief Internal helper to enforce capacity limits. Must be called with lock held.
     */
    void ensure_capacity_locked(uint64_t required_bytes) {
        if (required_bytes > capacity_) {
            std::cerr << "[ITS] CRITICAL: Resource request (" << required_bytes 
                      << " bytes) exceeds total cache capacity (" << capacity_ << " bytes).\n";
            return; // Graceful failure: Cannot cache this item, treat as uncacheable bypass
        }

        // Evict Least Recently Used (back of the list) until space is available
        while (current_usage_ + required_bytes > capacity_ && !lru_list_.empty()) {
            uint64_t evict_id = lru_list_.back();
            uint64_t evict_size = get_resource_size_callback_(evict_id);
            
            current_usage_ -= evict_size;
            lru_map_.erase(evict_id);
            lru_list_.pop_back();
        }
    }

    uint64_t capacity_;
    uint64_t current_usage_;
    
    std::list<uint64_t> lru_list_; 
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_map_; 
    
    uint64_t hits_;
    uint64_t misses_;

    mutable std::mutex cache_mutex_;
    std::function<uint64_t(uint64_t)> get_resource_size_callback_;
};

} // namespace synapse::its