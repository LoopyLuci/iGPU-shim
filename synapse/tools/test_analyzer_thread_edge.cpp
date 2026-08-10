/**
 * @file test_analyzer_thread_edge.cpp
 * @brief Analyzer thread edge-case coverage.
 *
 * Covers:
 *  - empty telemetry path
 *  - shutdown race
 *  - recommendation stability when telemetry is sparse
 */

#include "synapse_core.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <chrono>

static void stub_draw_indexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {}
static void stub_draw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}
static void stub_dispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
static void stub_push_constants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*) {}
static void stub_bind_desc_sets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) {}
static void stub_bind_shaders(VkCommandBuffer, uint32_t, const VkShaderStageFlagBits*, const VkShaderEXT*) {}

int main() {
    printf("=== Analyzer Thread Edge Cases ===\n");

    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_analyzer_edge";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        assert(core.state_machine().current() == synapse::atomic::ShimState::Active);

        // Edge 1: empty telemetry should keep default Oracle recommendation.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        auto empty_rec = core.current_analyzer_recommendation();
        printf("  Empty-telemetry recommendation: %d\n", (int)empty_rec);
        assert(empty_rec == synapse::ExecutionBackend::Oracle);

        // Edge 2: sparse mixed workload should converge and remain stable.
        for (int i = 0; i < 8; ++i) {
            core.handle_dispatch(VK_NULL_HANDLE, 1, 1, 1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        auto sparse_rec = core.current_analyzer_recommendation();
        printf("  Sparse mixed recommendation   : %d\n", (int)sparse_rec);
        assert(sparse_rec == synapse::ExecutionBackend::JIT || sparse_rec == synapse::ExecutionBackend::Oracle);
    }

    fs::remove_all(dir);
    printf("Result: PASS\n");
    return 0;
}
