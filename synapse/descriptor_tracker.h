// ============================================================================
// synapse/descriptor_tracker.h
// Project Synapse – Resource Access & Binding Tracking for ITS
// ============================================================================
#pragma once

#include <vector>
#include <unordered_map>
#include <cstdint>

namespace synapse::replayer {

/**
 * @struct ResourceMetadata
 * @brief Metadata required for ITS pre-fetching and residency logic.
 */
struct ResourceMetadata {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 1;
    bool is_texture = false;
};

/**
 * @struct DescriptorBinding
 * @brief Represents a single slot in a descriptor set.
 */
struct DescriptorBinding {
    uint64_t resource_handle = 0;
    uint32_t binding_type = 0; // VK_DESCRIPTOR_TYPE_...
};

/**
 * @class DescriptorTracker
 * @brief Tracks bound resources to build the ITS access model.
 */
class DescriptorTracker {
public:
    // Recorded during vkCreateImage / vkCreateBuffer
    void register_resource(uint64_t handle, uint32_t w, uint32_t h, uint32_t mips, bool is_tex) {
        registry_[handle] = { w, h, mips, is_tex };
    }

    // Recorded during vkUpdateDescriptorSets
    void update_set(uint64_t set_handle, uint32_t binding, uint64_t resource_handle) {
        sets_[set_handle][binding] = { resource_handle, 0 /* Type from layout */ };
    }

    // Recorded during vkCmdBindDescriptorSets
    void bind_sets(uint64_t cmdBuffer, uint32_t firstSet, const std::vector<uint64_t>& sets) {
        auto& active = active_bindings_[cmdBuffer];
        for (size_t i = 0; i < sets.size(); ++i) {
            active[firstSet + i] = sets[i];
        }
    }

    /**
     * @brief Summarizes the memory footprint of the current draw call.
     */
    void get_current_signature(uint64_t cmdBuffer, uint32_t& tex_count, uint32_t& buf_count) {
        tex_count = 0;
        buf_count = 0;
        auto it = active_bindings_.find(cmdBuffer);
        if (it == active_bindings_.end()) return;

        for (auto const& [slot, set_handle] : it->second) {
            for (auto const& [binding, desc] : sets_[set_handle]) {
                auto res_it = registry_.find(desc.resource_handle);
                if (res_it != registry_.end()) {
                    if (res_it->second.is_texture) tex_count++;
                    else buf_count++;
                }
            }
        }
    }

private:
    std::unordered_map<uint64_t, ResourceMetadata> registry_;
    // SetHandle -> BindingSlot -> Resource
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, DescriptorBinding>> sets_;
    // CmdBuffer -> SetSlot -> SetHandle
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint64_t>> active_bindings_;
};

} // namespace synapse::replayer