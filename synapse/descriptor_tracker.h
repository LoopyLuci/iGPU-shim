// ============================================================================
// synapse/descriptor_tracker.h
// Project Synapse – Resource Metadata & Binding State Tracking
// ============================================================================
#pragma once

#include <unordered_map>
#include <vector>
#include <cstdint>

namespace synapse::replayer {

/**
 * @struct ResourceMetadata
 * @brief Immutable properties of a GPU resource.
 */
struct ResourceMetadata {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t mip_levels = 1;
    uint16_t array_layers = 1;
    bool is_texture = false;
    uint64_t size_bytes = 0; // Estimated for bandwidth calculation
};

/**
 * @class DescriptorTracker
 * @brief Tracks the 'What' and 'Where' of resource bindings.
 */
class DescriptorTracker {
public:
    // Recorded during vkCreateImage/Buffer
    void register_resource(uint64_t handle, const ResourceMetadata& meta) {
        registry_[handle] = meta;
    }

    // Recorded during vkUpdateDescriptorSets
    void update_descriptor_set(uint64_t set, uint32_t binding, uint64_t resource) {
        set_contents_[set][binding] = resource;
    }

    // Recorded during vkCmdBindDescriptorSets
    void bind_set_to_cmd(uint64_t cmd_buffer, uint32_t slot, uint64_t set) {
        cmd_bindings_[cmd_buffer][slot] = set;
    }

    /**
     * @brief Generates a snapshot of currently bound resources for the Analyzer.
     */
    void snapshot_bindings(uint64_t cmd_buffer, uint32_t& tex_count, uint32_t& buf_count) {
        tex_count = 0;
        buf_count = 0;
        
        auto& bound_sets = cmd_bindings_[cmd_buffer];
        for (auto const& [slot, set_handle] : bound_sets) {
            for (auto const& [binding, res_handle] : set_contents_[set_handle]) {
                if (registry_[res_handle].is_texture) tex_count++;
                else buf_count++;
            }
        }
    }

private:
    std::unordered_map<uint64_t, ResourceMetadata> registry_;
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint64_t>> set_contents_;
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint64_t>> cmd_bindings_;
};

} // namespace synapse::replayer