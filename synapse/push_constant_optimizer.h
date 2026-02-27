// ============================================================================
// synapse/push_constant_optimizer.h
// Project Synapse – Word-Mask Delta Tracking for Push Constants
// ============================================================================
#pragma once

#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>
#include "hai_bytecode_builder.h"

namespace synapse::optimizer {

constexpr uint32_t MAX_PUSH_CONSTANT_WORDS = 32; // 128 bytes / 4

/**
 * @struct PushConstantState
 * @brief Shadow state for a specific command buffer context.
 */
struct PushConstantState {
    std::array<uint32_t, MAX_PUSH_CONSTANT_WORDS> shadow_memory = {0};
    bool is_invalidated = true;
};

/**
 * @class PushConstantOptimizer
 * @brief Orchestrates delta-compressed push constant updates.
 */
class PushConstantOptimizer {
public:
    /**
     * @brief Processes a push constant update and emits the optimal HAI bytecode.
     * @param cmdBuffer Handle for the current command buffer.
     * @param data Raw byte data from the Vulkan API.
     * @param size Size in bytes.
     * @param builder The HAI bytecode builder for emission.
     */
    void process_update(uint64_t cmdBuffer, const uint8_t* data, uint32_t size, 
                        builder::HAIBytecodeBuilder& builder) {
        auto& state = contexts_[cmdBuffer];
        uint32_t word_count = size / 4;
        const uint32_t* new_values = reinterpret_cast<const uint32_t*>(data);

        uint32_t dirty_mask = 0;
        uint32_t changed_count = 0;
        std::vector<uint32_t> payload;

        for (uint32_t i = 0; i < word_count; ++i) {
            if (state.is_invalidated || state.shadow_memory[i] != new_values[i]) {
                dirty_mask |= (1 << i);
                state.shadow_memory[i] = new_values[i];
                payload.push_back(new_values[i]);
                changed_count++;
            }
        }

        // Logic Branch: Full update vs Delta update
        // Threshold: If more than 50% of the block changed, send full update for HW efficiency
        if (state.is_invalidated || changed_count > (word_count / 2)) {
            builder.emit_set_push_constant(data, size);
        } else if (changed_count > 0) {
            // Emit HAI Delta (Opcode 0xFF) targeting SET_PUSH_CONSTANT (Target 0x12)
            builder.emit_delta_update(0x12, dirty_mask, payload);
        }

        state.is_invalidated = false;
        total_updates_++;
        if (changed_count > 0) total_changes_++;
    }

    void invalidate(uint64_t cmdBuffer) { contexts_[cmdBuffer].is_invalidated = true; }

    float get_change_rate() const { 
        return total_updates_ > 0 ? static_cast<float>(total_changes_) / total_updates_ : 0.0f; 
    }

private:
    std::unordered_map<uint64_t, PushConstantState> contexts_;
    uint64_t total_updates_ = 0;
    uint64_t total_changes_ = 0;
};

} // namespace synapse::optimizer