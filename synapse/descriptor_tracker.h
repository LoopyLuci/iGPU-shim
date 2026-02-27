// ============================================================================
// synapse/descriptor_tracker.h
// Project Synapse – Resource Footprint Aggregation
// ============================================================================
#pragma once

#include <unordered_map>
#include <algorithm>
#include <cstdint>

namespace synapse::replayer {

/**
 * @struct ResourceFootprintStats
 * @brief Internal counters for aggregate telemetry.
 */
struct ResourceFootprintStats {
    uint64_t total_draws = 0;
    double   sum_textures = 0;
    double   sum_buffers = 0;
    double   sum_vram_bytes = 0;
    
    uint32_t max_textures = 0;
    uint32_t max_buffers = 0;
    uint64_t max_vram_bytes = 0;
};

class DescriptorTracker {
public:
    // ... existing registration and binding methods ...

    /**
     * @brief Updates aggregate stats for the current draw call.
     * Called during every vkCmdDraw/Dispatch in the shim.
     */
    void record_draw_telemetry(uint64_t cmd_buffer) {
        uint32_t tex_count = 0;
        uint32_t buf_count = 0;
        uint64_t vram_bytes = 0;

        // Traverse currently bound sets for this command buffer
        auto& bound_sets = cmd_bindings_[cmd_buffer];
        for (auto const& [slot, set_handle] : bound_sets) {
            for (auto const& [binding, res_handle] : set_contents_[set_handle]) {
                auto const& meta = registry_[res_handle];
                if (meta.is_texture) tex_count++;
                else buf_count++;
                vram_bytes += meta.size_bytes;
            }
        }

        // Update Watermarks (Defensive Programming: thread-safety if replayer is multi-threaded)
        stats_.total_draws++;
        stats_.sum_textures += tex_count;
        stats_.sum_buffers += buf_count;
        stats_.sum_vram_bytes += vram_bytes;

        stats_.max_textures = std::max(stats_.max_textures, tex_count);
        stats_.max_buffers = std::max(stats_.max_buffers, buf_count);
        stats_.max_vram_bytes = std::max(stats_.max_vram_bytes, vram_bytes);
    }

    /**
     * @brief Returns the final summary for MetricsExporter.
     */
    ResourceFootprintStats get_final_stats() const { return stats_; }

private:
    ResourceFootprintStats stats_;
    // ... existing registry and binding maps ...
};

} // namespace synapse::replayer