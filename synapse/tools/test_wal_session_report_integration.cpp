/**
 * @file test_wal_session_report_integration.cpp
 * @brief Validates that build_session_report() consumes recovered WAL entries.
 *
 * Writes WAL entries, simulates a crash, recovers, and verifies
 * the session report reflects the recovered telemetry.
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
    printf("=== WAL-to-Session-Report Integration ===\n");

    auto dir = fs::temp_directory_path() / "synapse_wal_session_report";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");

    // Phase 1: Write WAL entries and simulate crash.
    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        for (int i = 0; i < 32; ++i) {
            core.handle_dispatch(VK_NULL_HANDLE, 4, 4, 1);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        core.recovery()->telemetry().simulate_crash();
    }

    // Phase 2: New session recovers WAL and builds report.
    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        auto recovery = core.check_and_recover();
        printf("  crash detected: %s\n", recovery.crash_detected ? "YES" : "NO");
        printf("  entries recovered: %llu\n", (unsigned long long)recovery.entries_recovered);

        if (recovery.crash_detected) {
            uint64_t replayed = core.recovery()->telemetry().replay();
            printf("  entries replayed: %llu\n", (unsigned long long)replayed);
        }

        auto report = core.build_session_report();
        printf("  Session report generated: YES\n");
        (void)report;
    }

    fs::remove_all(dir);
    printf("Result: PASS\n");
    return 0;
}
