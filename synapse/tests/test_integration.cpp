// ============================================================================
// synapse/tests/test_integration.cpp
// Integration tests for the full init → intercept → recover cycle.
// ============================================================================
#include "../synapse_core.h"
#include "../hotreload/config_watcher.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { printf("  %-55s ", name); } while(0)
#define PASS() do { printf("PASS\n"); g_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); g_failed++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// Minimal Vulkan function pointers (stubs)
static void stub_draw_indexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {}
static void stub_draw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}
static void stub_dispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) {}
static void stub_push_constants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*) {}
static void stub_bind_desc_sets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) {}
static void stub_bind_shaders(VkCommandBuffer, uint32_t, const VkShaderStageFlagBits*, const VkShaderEXT*) {}

// ============================================================================
// DEBUG: WAL crash simulation (isolated from SynapseCore)
// ============================================================================
static void test_wal_debug() {
    TEST("DEBUG: WAL crash simulation (isolated)");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "wal_debug_test";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    {
        synapse::recovery::CrashRecoveryManager rm(dir.string());
        rm.telemetry().write(synapse::atomic::WALEventType::DrawIndexed);
        rm.telemetry().write(synapse::atomic::WALEventType::Dispatch);
        rm.telemetry().simulate_crash();
    }

    auto wal_size = fs::file_size(dir / "synapse.wal");
    std::cerr << "\n    WAL size: " << wal_size << " bytes (expected " << (2*264) << ")" << std::endl;

    {
        synapse::recovery::CrashRecoveryManager rm(dir.string());
        bool crashed = rm.check_for_crash();
        std::cerr << "    Crash detected: " << crashed << std::endl;
        if (crashed) {
            auto result = rm.recover();
            std::cerr << "    Recovered: " << result.entries_recovered << std::endl;
            ASSERT(result.entries_recovered == 2, "should recover 2 entries");
        } else {
            FAIL("should detect crash");
        }
    }
    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Integration test: Full init → state transitions → WAL → crash → recover
// ============================================================================
static void test_full_lifecycle() {
    TEST("Integration: full lifecycle init→active→shutdown");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_integration_lifecycle";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        ASSERT(core.state_machine().current() == synapse::atomic::ShimState::Active,
               "state should be Active");
        ASSERT(core.config().power_budget() == 15, "default power budget");
        ASSERT(core.is_feature_available("jit"), "jit should be available");
        ASSERT(core.is_feature_available("hai"), "hai should be available");
        ASSERT(core.is_feature_available("ml"), "ml should be available");

        auto report = core.build_session_report();
        ASSERT(report.backend_routing.total_draw_calls == 0, "no draw calls yet");
    }
    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Integration test: Crash recovery through SynapseCore
// ============================================================================
static void test_crash_recovery() {
    TEST("Integration: crash detection and recovery via SynapseCore");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_integration_crash";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    // Session 1: write WAL entries, then "crash"
    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        core.recovery()->telemetry().write(synapse::atomic::WALEventType::DrawIndexed);
        core.recovery()->telemetry().write(synapse::atomic::WALEventType::Dispatch);
        core.recovery()->telemetry().simulate_crash();
    }

    // Session 2: should detect crash and recover
    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        auto info = core.check_and_recover();
        ASSERT(info.crash_detected, "should detect crash");
        ASSERT(info.entries_recovered == 2, "should recover 2 entries");
    }

    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Integration test: Graceful degradation
// ============================================================================
static void test_degradation_lifecycle() {
    TEST("Integration: degradation lifecycle");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_integration_degrade";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    synapse::SynapseCore core(
        stub_draw_indexed, stub_draw, stub_dispatch,
        stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
        dir.string());

    ASSERT(core.is_feature_available("jit"), "jit available initially");
    core.degradation().handle_error("jit", 1);
    ASSERT(core.is_feature_available("jit"), "jit still available when degraded");
    core.degradation().handle_error("jit", 2);
    ASSERT(!core.is_feature_available("jit"), "jit disabled on persistent error");
    core.degradation().handle_error("jit", 0);
    ASSERT(core.is_feature_available("jit"), "jit recovered");
    core.degradation().reset_all();
    ASSERT(core.is_feature_available("jit"), "jit available after reset");

    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Integration test: User profile
// ============================================================================
static void test_user_profile_integration() {
    TEST("Integration: user profile presets → config sync");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_integration_profile";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    synapse::SynapseCore core(
        stub_draw_indexed, stub_draw, stub_dispatch,
        stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
        dir.string());

    // Apply battery-saver preset
    core.apply_preset("battery-saver");
    auto plan = core.user_profile().plan();
    ASSERT(plan.power_budget_watts == 8, "battery-saver power");
    ASSERT(plan.fan_curve == "silent", "battery-saver fan");
    // Verify config_ was synced
    auto cfg = core.config().read();
    ASSERT(cfg.power_budget_watts == 8, "config synced from battery-saver");

    // Apply performance preset
    core.apply_preset("performance");
    plan = core.user_profile().plan();
    ASSERT(plan.power_budget_watts == 25, "performance power");
    cfg = core.config().read();
    ASSERT(cfg.power_budget_watts == 25, "config synced from performance");

    // Verify data_dir accessor
    ASSERT(!core.data_dir().empty(), "data_dir should be non-empty");

    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Integration test: Schema migration
// ============================================================================
static void test_schema_migration_integration() {
    TEST("Integration: schema versioning");
    synapse::protocol::SchemaMigration sm;
    sm.register_migration(1, 2, [](const auto& data) {
        std::vector<uint8_t> r = data; r.push_back(0x02); return r;
    });
    sm.register_migration(2, 3, [](const auto& data) {
        std::vector<uint8_t> r = data; r.push_back(0x03); return r;
    });
    std::vector<uint8_t> data = {0x01, 0x02};
    auto migrated = sm.migrate(data, 1, 3);
    ASSERT(migrated.size() == 4, "wrong size");
    ASSERT(migrated[2] == 0x02, "missing v2 marker");
    ASSERT(migrated[3] == 0x03, "missing v3 marker");
    ASSERT(sm.latest_version() == 3, "wrong latest version");
    PASS();
}

// ============================================================================
// Integration test: Concurrent state transitions
// ============================================================================
static void test_concurrent_state_transitions() {
    TEST("Integration: concurrent state transitions");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_integration_concurrent";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    synapse::SynapseCore core(
        stub_draw_indexed, stub_draw, stub_dispatch,
        stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
        dir.string());

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&core, i]() {
            core.degradation().handle_error("jit", i % 2 == 0 ? 1 : 0);
        });
    }
    for (auto& t : threads) t.join();

    ASSERT(core.state_machine().current() == synapse::atomic::ShimState::Active,
           "state should remain Active");
    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Integration test: WAL write/read cycle
// ============================================================================
static void test_wal_write_read_cycle() {
    TEST("Integration: WAL write→read→replay cycle");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_integration_wal";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        for (int i = 0; i < 10; ++i) {
            core.recovery()->telemetry().write(synapse::atomic::WALEventType::DrawIndexed);
        }
        ASSERT(core.recovery()->telemetry().write_count() == 10, "wrong write count");
    }

    {
        synapse::SynapseCore core(
            stub_draw_indexed, stub_draw, stub_dispatch,
            stub_push_constants, stub_bind_desc_sets, stub_bind_shaders,
            dir.string());

        auto info = core.check_and_recover();
        ASSERT(!info.crash_detected, "should not detect crash after clean shutdown");
    }

    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Integration test: ConfigWatcher hot-reload
// ============================================================================
static void test_config_watcher_hotreload() {
    TEST("Integration: ConfigWatcher live reload");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_integration_configwatcher";
    fs::create_directories(dir);
    fs::remove(dir / "synapse.wal");
    fs::remove(dir / "synapse_recovery.meta");

    // Create a config file manually
    auto config_path = dir / "config.toml";
    {
        std::ofstream ofs(config_path);
        ofs << "power_budget = 10\n";
        ofs << "thermal_target = 75\n";
        ofs << "ml_aggressive = false\n";
        ofs << "jit_enabled = true\n";
        ofs << "hai_enabled = false\n";
    }

    // Create ConfigWatcher and verify callback fires
    synapse::hotreload::ConfigWatcher watcher(config_path);
    bool callback_fired = false;
    std::string received_content;

    watcher.on_change([&](const std::string& content) {
        callback_fired = true;
        received_content = content;
    });

    // Use force_reload to bypass mtime (tests callback mechanism directly)
    watcher.force_reload();
    ASSERT(callback_fired, "callback should fire on force_reload");
    ASSERT(received_content.find("power_budget = 10") != std::string::npos,
           "callback should receive file content");

    // Modify file and force reload again
    callback_fired = false;
    {
        std::ofstream ofs(config_path, std::ios::trunc);
        ofs << "power_budget = 20\n";
        ofs << "thermal_target = 90\n";
    }
    watcher.force_reload();
    ASSERT(callback_fired, "callback should fire after config change");
    ASSERT(received_content.find("power_budget = 20") != std::string::npos,
           "callback should receive updated content");

    // Verify watcher can start/stop
    watcher.start();
    ASSERT(watcher.is_watching(), "watcher should be running");
    watcher.stop();
    ASSERT(!watcher.is_watching(), "watcher should be stopped");

    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Integration test: SchemaMigration backward compat
// ============================================================================
static void test_schema_backward_compat() {
    TEST("Integration: schema backward compatibility");
    synapse::protocol::SchemaMigration sm;
    sm.register_migration(0, 1, [](const auto& data) {
        std::vector<uint8_t> r = data;
        r.push_back(0xAA);
        return r;
    });

    // v1 can read v1 data
    ASSERT(sm.is_compatible(1, 1), "v1 should read v1");
    // v1 cannot read v0 (two versions back)
    ASSERT(!sm.is_compatible(1, 0), "v1 should not read v0 (two versions back)");
    // v0 can read v0
    ASSERT(sm.is_compatible(0, 0), "v0 should read v0");

    // Migration path exists
    ASSERT(sm.has_path(0, 1), "migration path v0→v1 should exist");

    // Migrate and verify
    std::vector<uint8_t> data = {0x01, 0x02};
    auto migrated = sm.migrate(data, 0, 1);
    ASSERT(migrated.size() == 3, "migrated data should be larger");
    ASSERT(migrated[2] == 0xAA, "migration marker should be present");

    PASS();
}

// ============================================================================
// main
// ============================================================================
int main() {
    printf("=== Project Synapse — Integration Tests ===\n\n");

    test_wal_debug();
    test_full_lifecycle();
    test_crash_recovery();
    test_degradation_lifecycle();
    test_user_profile_integration();
    test_schema_migration_integration();
    test_concurrent_state_transitions();
    test_wal_write_read_cycle();
    test_config_watcher_hotreload();
    test_schema_backward_compat();

    printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
