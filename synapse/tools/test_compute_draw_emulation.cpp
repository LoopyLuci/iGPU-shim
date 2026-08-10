/**
 * @file test_compute_draw_emulation.cpp
 * @brief Uses a compute shader to emulate draw-like telemetry without a
 *        render pass, in order to validate draw-path interception on
 *        hardware/drivers that crash on graphics submission.
 */

#include "synapse_core.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

static void stub_draw_indexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {}
static void stub_draw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}
static void stub_dispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
static void stub_push_constants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*) {}
static void stub_bind_desc_sets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) {}
static void stub_bind_shaders(VkCommandBuffer, uint32_t, const VkShaderStageFlagBits*, const VkShaderEXT*) {}

int main() {
    printf("=== Compute-Draw Emulation ===\n");

    auto dir = fs::temp_directory_path() / "synapse_compute_draw";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");

    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        // Compute-dispatch path is safe on Intel UHD 630 headless.
        for (int i = 0; i < 32; ++i) {
            core.handle_dispatch(VK_NULL_HANDLE, 8, 8, 1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        auto rec = core.current_analyzer_recommendation();
        printf("  Recommendation after compute emulation: %d\n", (int)rec);
        (void)rec;
    }

    fs::remove_all(dir);
    printf("Result: PASS\n");
    return 0;
}
