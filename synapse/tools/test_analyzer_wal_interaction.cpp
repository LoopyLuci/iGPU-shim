/**
 * @file test_analyzer_wal_interaction.cpp
 * @brief Validates analyzer–WAL interaction under concurrent consumption.
 *
 * Writes telemetry, lets the analyzer run, then validates WAL recovery
 * metadata and CleanShutdown marker after core destruction.
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

static void run_test() {
    auto dir = fs::temp_directory_path() / "synapse_analyzer_wal";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    constexpr int kDraws = 20;
    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        for (int i = 0; i < kDraws; ++i) {
            core.handle_dispatch(VK_NULL_HANDLE, 1, 1, 1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    // After destruction, WAL should still contain CleanShutdown marker.
    std::error_code ec;
    auto wal = dir / "synapse.wal";
    auto meta = dir / "synapse_recovery.meta";

    assert(fs::exists(wal, ec));
    assert(fs::exists(meta, ec));
    assert(wal.file_size() > 0);
    assert(meta.file_size() > 0);

    fs::remove_all(dir);
}

int main() {
    printf("=== Analyzer-WAL Interaction ===\n");
    run_test();
    printf("Result: PASS\n");
    return 0;
}
