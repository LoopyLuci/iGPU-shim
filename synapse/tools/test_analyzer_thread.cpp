/**
 * @file test_analyzer_thread.cpp
 * @brief Validates the background Analyzer thread consumes telemetry and emits
 *        recommendations without real graphics draw submissions.
 *
 * This exercises the compute/memory-bandwidth path by pushing synthetic
 * WorkloadSignature samples through SynapseCore and asserting that the
 * Analyzer backend choice evolves from the default Oracle to JIT/HAI based
 * on workload characteristics.
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
    printf("=== Analyzer Thread Live Exercise ===\n");

    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_analyzer_live";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        assert(core.state_machine().current() == synapse::atomic::ShimState::Active);

        // Seed the analyzer with a light workload; expect Oracle by default.
        for (int i = 0; i < 64; ++i) {
            core.notify_bind_pipeline(VK_NULL_HANDLE, VK_NULL_HANDLE, 0);
            core.handle_draw_indexed(VK_NULL_HANDLE, 3, 1, 0, 0, 0);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        auto first = core.current_analyzer_recommendation();

        // Heavy/compute-like workload: large instruction estimate + dispatch.
        for (int i = 0; i < 64; ++i) {
            core.handle_dispatch(VK_NULL_HANDLE, 8, 8, 1);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        auto second = core.current_analyzer_recommendation();

        printf("  Initial recommendation : %d\n", (int)first);
        printf("  After heavy dispatch   : %d\n", (int)second);

        assert(first == synapse::ExecutionBackend::Oracle);
        assert(second == synapse::ExecutionBackend::JIT);
        assert(core.state_machine().current() == synapse::atomic::ShimState::Active);
    }

    fs::remove_all(dir);
    printf("Result: PASS\n");
    return 0;
}
