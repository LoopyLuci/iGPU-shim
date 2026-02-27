// ============================================================================
// synapse/its_cache_controller.h
// Project Synapse – Predictive Mip-map Residency with LRU Eviction
// ============================================================================
#pragma once

#include <list>
#include <unordered_map>
#include <cstdint>
#include <iostream>
#include "descriptor_tracker.h"

namespace synapse::its {

/**
 * @class ITSCacheController
 * @brief Manages the iGPU Synaptic Cache capacity and residency.
 */
class ITSCacheController {
public:
    explicit ITSCacheController(uint64_t capacity_bytes) 
        : capacity_(capacity_bytes), current_usage_(0), hits_(0), misses_(0) {}

    /**
     * @brief Marks a resource as accessed and updates its position in the LRU.
     * @return True if the resource was already resident (Hit), False otherwise (Miss).
     */
    bool access_resource(uint64_t resource_id, const replayer::ResourceMetadata& meta, uint64_t current_frame) {
        if (lru_map_.find(resource_id) != lru_map_.end()) {
            // Cache Hit: Move to front (most recently used)
            lru_list_.erase(lru_map_[resource_id]);
            lru_list_.push_front(resource_id);
            lru_map_[resource_id] = lru_list_.begin();
            hits_++;
            return true;
        }

        // Cache Miss: Must "load" into cache
        misses_++;
        ensure_capacity(meta.size_bytes);
        
        lru_list_.push_front(resource_id);
        lru_map_[resource_id] = lru_list_.begin();
        current_usage_ += meta.size_bytes;
        
        return false;
    }

    /**
     * @brief Evicts resources until requested space is available.
     */
    void ensure_capacity(uint64_t required_bytes) {
        // Defensive: If a single resource is larger than the entire cache
        if (required_bytes > capacity_) {
            std::cerr << "[ITS] Warning: Resource exceeds total cache capacity.\n";
            return;
        }

        while (current_usage_ + required_bytes > capacity_ && !lru_list_.empty()) {
            uint64_t evict_id = lru_list_.back();
            
            // We need a way to look up the size of the evicted resource.
            // In the full implementation, this is tied to the ResourceRegistry.
            // For now, we assume a metadata callback or shared registry.
            uint64_t evict_size = get_resource_size_callback_(evict_id);
            
            current_usage_ -= evict_size;
            lru_map_.erase(evict_id);
            lru_list_.pop_back();
        }
    }

    // Telemetry API
    uint64_t get_hits() const { return hits_; }
    uint64_t get_misses() const { return misses_; }
    float get_hit_rate() const { 
        uint64_t total = hits_ + misses_;
        return total > 0 ? (float)hits_ / total : 0.0f; 
    }

private:
    uint64_t capacity_;
    uint64_t current_usage_;
    
    std::list<uint64_t> lru_list_; // Tracks recency
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_map_; // O(1) Access
    
    uint64_t hits_;
    uint64_t misses_;

    // Dependency Injection for size lookups
    std::function<uint64_t(uint64_t)> get_resource_size_callback_;
};

} // namespace synapse::its