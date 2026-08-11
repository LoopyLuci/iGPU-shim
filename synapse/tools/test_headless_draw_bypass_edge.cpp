/**
 * @file test_headless_draw_bypass_edge.cpp
 * @brief Edge-case headless bypass tests.
 *
 * Covers:
 *  - empty workload: construction/destruction with no draw/dispatch calls
 *  - sequential instances: multiple SynapseCore objects in the same data dir
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
    printf("=== Headless Draw Bypass Edge Cases ===\n");

    auto dir = fs::temp_directory_path() / "synapse_headless_draw_edge";
    fs::create_directories(dir);
    auto wal_path = dir / "synapse.wal";
    fs::remove(wal_path);

    const uint64_t initial_size = wal_size(wal_path);

    // Edge A: empty workload should not grow WAL.
    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());
        // No handle_* calls.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const uint64_t after_empty = wal_size(wal_path);
    printf("  Empty workload WAL size: %llu -> %llu bytes\n",
           (unsigned long long)initial_size, (unsigned long long)after_empty);

    // Edge B: sequential instances should grow WAL monotonically beyond empty baseline.
    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());
        core.handle_dispatch(VK_NULL_HANDLE, 4, 4, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const uint64_t after_first = wal_size(wal_path);
    const uint64_t first_delta = after_first > after_empty ? after_first - after_empty : 0ull;
    printf("  Sequential instance 1 WAL size: %llu bytes (+%llu)\n",
           (unsigned long long)after_first, (unsigned long long)first_delta);

    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());
        core.handle_dispatch(VK_NULL_HANDLE, 8, 8, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const uint64_t after_second = wal_size(wal_path);
    const uint64_t second_delta = after_second > after_first ? after_second - after_first : 0ull;
    printf("  Sequential instance 2 WAL size: %llu bytes (+%llu)\n",
           (unsigned long long)after_second, (unsigned long long)second_delta);

    fs::remove_all(dir);

    assert(after_empty - initial_size <= sizeof(atomic::WALEntry) && "Empty workload should not write more than one WAL entry");
    assert(first_delta > 0 && "First instance should grow WAL beyond empty baseline");
    assert(second_delta > 0 && "Second instance should grow WAL monotonically");
    printf("Result: PASS\n");
    return 0;
}
