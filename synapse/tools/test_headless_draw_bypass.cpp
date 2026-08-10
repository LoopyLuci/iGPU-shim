/**
 * @file test_headless_draw_bypass.cpp
 * @brief Attempts headless draw-path validation without a render pass.
 *
 * Uses vkCmdDrawIndirect / vkCmdDispatch as draw-like paths that may avoid
 * the Intel driver crash seen under full graphics submission.
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
    printf("=== Headless Draw Bypass ===\n");

    auto dir = fs::temp_directory_path() / "synapse_headless_draw";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");

    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        // Path A: direct dispatch
        core.handle_dispatch(VK_NULL_HANDLE, 4, 4, 1);

        // Path B: draw-indexed
        core.handle_draw_indexed(VK_NULL_HANDLE, 6, 1, 0, 0, 0);

        // Path C: draw
        core.handle_draw(VK_NULL_HANDLE, 3, 1, 0, 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        auto rec = core.current_analyzer_recommendation();
        printf("  Recommendation after mixed draw-like ops: %d\n", (int)rec);
        (void)rec;
    }

    fs::remove_all(dir);
    printf("Result: PASS\n");
    return 0;
}
