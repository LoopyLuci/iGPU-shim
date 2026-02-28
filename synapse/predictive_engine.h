// ============================================================================
// synapse/predictive_engine.h
// Project Synapse – Synchronous Temporal-Locality ITS Predictor
// ============================================================================
#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <iostream>

#include "descriptor_tracker.h"
#include "its_cache_controller.h"
#include "sync_manager.h"
#include "hai_bytecode_builder.h"

namespace synapse::its {

/**
 * @class PredictiveEngine
 * @brief Synchronously predicts and pre-fetches resources based on temporal locality.
 */
class PredictiveEngine {
public:
    PredictiveEngine(uint32_t temporal_window_frames, 
                     ITSCacheController& cache, 
                     sync::SyncManager& sync, 
                     builder::HAIBytecodeBuilder& builder)
        : temporal_window_(temporal_window_frames), 
          cache_controller_(cache), 
          sync_manager_(sync), 
          builder_(builder) {}

    /**
     * @brief Evaluates current bindings and issues prefetch commands for future frames.
     * Called synchronously after every draw/dispatch in the GFXRConsumer.
     */
    void evaluate_and_predict(uint64_t current_frame, const std::vector<uint64_t>& bound_resources, const replayer::DescriptorTracker& tracker) {
        for (uint64_t resource_id : bound_resources) {
            // 1. Score keeping: If we are using it now, was it prefetched?
            if (active_predictions_.count(resource_id)) {
                stats_.accurate_predictions++;
                active_predictions_.erase(resource_id);
            }

            // 2. Heuristic: Temporal Locality
            // If it's used this frame, we predict it will be used in the next frames.
            // We request the "next" mip level (higher detail) to simulate streaming in.
            auto meta = tracker.get_metadata(resource_id);
            if (meta.is_texture && meta.mip_levels > 1) {
                
                // Defensive: Don't spam predictions if one is already in-flight
                if (active_predictions_.count(resource_id) == 0) {
                    
                    // Simple MVP logic: Request the whole resource or next logical mip
                    // For V1, we simulate requesting the next mip level (Mip 0 is highest detail)
                    uint32_t target_mip = (meta.resident_mips > 0) ? meta.resident_mips - 1 : 0;
                    uint64_t required_bytes = meta.size_bytes; // Simplified for MVP

                    // Simulate the cache request
                    bool is_resident = cache_controller_.access_resource(resource_id, required_bytes);
                    
                    if (!is_resident) {
                        // Cache Miss -> Emit Prefetch & Setup Fences
                        uint64_t target_fence = current_frame + temporal_window_;
                        
                        sync_manager_.mark_pending_load(resource_id, target_fence);
                        builder_.emit_prefetch_hint(resource_id, target_mip, meta.mip_levels);
                        
                        active_predictions_[resource_id] = current_frame;
                        stats_.total_predictions++;
                    }
                }
            }
        }

        // 3. Prune wasted predictions (Prefetched but not used within the window + margin)
        std::vector<uint64_t> to_prune;
        for (auto const& [res_id, predicted_frame] : active_predictions_) {
            if (current_frame > predicted_frame + temporal_window_ + 5) { // 5 frame grace period
                stats_.wasted_predictions++;
                to_prune.push_back(res_id);
            }
        }
        for (uint64_t res_id : to_prune) {
            active_predictions_.erase(res_id);
        }
    }

    /**
     * @struct PredictionStats
     * @brief Telemetry for the report.json
     */
    struct PredictionStats {
        uint64_t total_predictions = 0;
        uint64_t accurate_predictions = 0;
        uint64_t wasted_predictions = 0;
    };

    PredictionStats get_stats() const { return stats_; }

private:
    uint32_t temporal_window_;
    ITSCacheController& cache_controller_;
    sync::SyncManager& sync_manager_;
    builder::HAIBytecodeBuilder& builder_;

    // Tracks resources that were prefetched to score accuracy (ResourceID -> Frame Prefetched)
    std::unordered_map<uint64_t, uint64_t> active_predictions_;
    
    PredictionStats stats_;
};

} // namespace synapse::its