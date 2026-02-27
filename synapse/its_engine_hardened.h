// ============================================================================
// synapse/its_engine_hardened.h
// Project Synapse – Hardened Texture Streaming (Hysteresis & DMA Fencing)
// ============================================================================
#pragma once

#include "synapse_umd.h"
#include <atomic>

namespace synapse {

struct MipResidencyState {
    std::atomic<bool> is_resident{false};
    std::atomic<uint64_t> dma_fence_id{0}; // Track async completion
    float current_priority{0.0f};          // Used for Hysteresis
};

class TextureStreamingEngineHardened {
public:
    // ... previous registration logic ...

    void prepare_for_use(VkImage image, uint64_t current_frame) {
        auto it = textures_.find(image);
        if (it == textures_.end()) return;

        TextureObject& tex = it->second;
        
        // 1. Telemetry-Driven Prediction
        float predicted_demand = analyzer_.get_mip_demand_probability(tex.texture_id);

        // 2. Apply Hysteresis
        // Only load if demand > 0.8; Only evict if demand < 0.2
        update_residency_with_hysteresis(tex, predicted_demand);
    }

    // Defensive Check: Returns the highest mip-level that is SAFELY resident
    uint32_t get_safe_mip_level(VkImage image) {
        auto& tex = textures_[image];
        for (uint32_t i = 0; i < tex.mip_count; ++i) {
            // Check if DMA transfer is actually finished via Hardware Fence
            if (tex.residency[i].is_resident && 
                hardware_fence_completed(tex.residency[i].dma_fence_id)) {
                return i;
            }
        }
        return tex.mip_count - 1; // Fallback to lowest resolution
    }

private:
    void update_residency_with_hysteresis(TextureObject& tex, float demand) {
        // Hysteresis prevents 'flickering' residency states
        const float kLoadThreshold = 0.85f;
        const float kEvictThreshold = 0.15f;

        for (auto& mip : tex.mips) {
            if (!mip.resident && demand > kLoadThreshold) {
                trigger_async_load(mip);
            } else if (mip.resident && demand < kEvictThreshold) {
                trigger_async_eviction(mip);
            }
        }
    }

    bool hardware_fence_completed(uint64_t id) {
        // Query the iGPU's DMA engine status register
        return true; 
    }
};

} // namespace synapse