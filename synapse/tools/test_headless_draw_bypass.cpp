/**
 * @file test_headless_draw_bypass.cpp
 * @brief Attempts headless draw-path validation without a render pass.
 *
 * Uses vkCmdDrawIndirect / vkCmdDispatch as draw-like paths that may avoid
 * the Intel driver crash seen under full graphics submission.
 *
 * This deeper-dive variant logs counts and whether the WAL grew, so we
 * can confirm the headless path actually advances telemetry.
 */

#include "synapse_core.h"
#include "../atomic/atomic_telemetry.h"

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

static uint64_t wal_size(const fs::path& wal_path) {
    std::error_code ec;
    auto sz = fs::file_size(wal_path, ec);
    if (ec) return 0;
    return sz;
}

int main() {
    printf("=== Headless Draw Bypass ===\n");

    auto dir = fs::temp_directory_path() / "synapse_headless_draw";
    fs::create_directories(dir);
    auto wal_path = dir / "synapse.wal";
    fs::remove(wal_path);

    const uint64_t initial_size = wal_size(wal_path);
    int dispatch_ok = 0;
    int draw_indexed_ok = 0;
    int draw_ok = 0;

    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        // Path A: direct dispatch
        core.handle_dispatch(VK_NULL_HANDLE, 4, 4, 1);
        ++dispatch_ok;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Path B: draw-indexed
        core.handle_draw_indexed(VK_NULL_HANDLE, 6, 1, 0, 0, 0);
        ++draw_indexed_ok;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Path C: draw
        core.handle_draw(VK_NULL_HANDLE, 3, 1, 0, 0);
        ++draw_ok;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        auto rec = core.current_analyzer_recommendation();
        printf("  Recommendation after mixed draw-like ops: %d\n", (int)rec);
        (void)rec;
    }

    const uint64_t final_size = wal_size(wal_path);
    printf("  Calls  : dispatch=%d, draw_indexed=%d, draw=%d\n",
           dispatch_ok, draw_indexed_ok, draw_ok);
    printf("  WAL size: %llu -> %llu bytes\n", (unsigned long long)initial_size, (unsigned long long)final_size);

    fs::remove_all(dir);

    assert(dispatch_ok == 1 && draw_indexed_ok == 1 && draw_ok == 1);
    assert(final_size > initial_size && "WAL did not grow after headless draw-like ops");
    printf("Result: PASS\n");
    return 0;
}
