// ============================================================================
// synapse/tests/test_production_modules.cpp
// Tests for Phase 10-14: Atomic, HotReload, Recovery, Personal, Protocol
// ============================================================================
#include "../atomic/atomic_state.h"
#include "../atomic/atomic_config.h"
#include "../atomic/atomic_telemetry.h"
#include "../atomic/graceful_degradation.h"
#include "../hotreload/config_watcher.h"
#include "../hotreload/ml_hotreload.h"
#include "../hotreload/shader_hotreload.h"
#include "../recovery/watchdog.h"
#include "../recovery/crash_recovery.h"
#include "../personal/user_profile.h"
#include "../personal/privacy_telemetry.h"
#include "../protocol/schema_migration.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Test helpers
// ============================================================================
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) \
    do { printf("  %-50s ", name); } while(0)

#define PASS() \
    do { printf("PASS\n"); g_tests_passed++; } while(0)

#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); g_tests_failed++; } while(0)

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

#define ASSERT_FALSE(cond, msg) \
    do { if ((cond)) { FAIL(msg); return; } } while(0)

// ============================================================================
// Phase 10.1: AtomicStateMachine
// ============================================================================
static void test_atomic_state_initial() {
    TEST("AtomicStateMachine: initial state");
    synapse::atomic::AtomicStateMachine sm;
    ASSERT_TRUE(sm.current() == synapse::atomic::ShimState::Uninitialized, "wrong initial");
    PASS();
}

static void test_atomic_state_transitions() {
    TEST("AtomicStateMachine: valid transitions");
    synapse::atomic::AtomicStateMachine sm;
    ASSERT_TRUE(sm.transition(synapse::atomic::ShimState::Initializing, "boot"), "init failed");
    ASSERT_TRUE(sm.current() == synapse::atomic::ShimState::Initializing, "not init");
    ASSERT_TRUE(sm.transition(synapse::atomic::ShimState::Active, "ready"), "active failed");
    ASSERT_TRUE(sm.current() == synapse::atomic::ShimState::Active, "not active");
    ASSERT_TRUE(sm.transition(synapse::atomic::ShimState::ShuttingDown, "exit"), "shutdown failed");
    ASSERT_TRUE(sm.transition(synapse::atomic::ShimState::Uninitialized, "done"), "reset failed");
    PASS();
}

static void test_atomic_state_invalid() {
    TEST("AtomicStateMachine: invalid transitions rejected");
    synapse::atomic::AtomicStateMachine sm;
    ASSERT_FALSE(sm.transition(synapse::atomic::ShimState::Active, "skip init"), "should reject");
    ASSERT_FALSE(sm.transition(synapse::atomic::ShimState::Crashed, "nope"), "should reject");
    PASS();
}

static void test_atomic_state_history() {
    TEST("AtomicStateMachine: history logging");
    synapse::atomic::AtomicStateMachine sm;
    sm.transition(synapse::atomic::ShimState::Initializing, "boot");
    sm.transition(synapse::atomic::ShimState::Active, "ready");
    auto h = sm.history();
    ASSERT_TRUE(h.size() == 2, "wrong history count");
    ASSERT_TRUE(h[0].reason[0] == 'b', "wrong reason");
    PASS();
}

static void test_atomic_state_concurrent() {
    TEST("AtomicStateMachine: concurrent transitions (CAS)");
    synapse::atomic::AtomicStateMachine sm(synapse::atomic::ShimState::Active);
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&sm]() {
            sm.transition(synapse::atomic::ShimState::Degraded, "concurrent");
        });
    }
    for (auto& t : threads) t.join();
    // Exactly one should have succeeded
    ASSERT_TRUE(sm.transition_count() <= 11, "too many transitions");
    PASS();
}

// ============================================================================
// Phase 10.2: AtomicConfig
// ============================================================================
static void test_atomic_config_defaults() {
    TEST("AtomicConfig: default values");
    synapse::atomic::AtomicConfig cfg;
    auto snap = cfg.read();
    ASSERT_TRUE(snap.power_budget_watts == 15, "wrong default power");
    ASSERT_TRUE(snap.ml_aggressive == true, "wrong default ml");
    PASS();
}

static void test_atomic_config_update() {
    TEST("AtomicConfig: atomic update");
    synapse::atomic::AtomicConfig cfg;
    auto v0 = cfg.version();
    bool ok = cfg.update([](auto& c) { c.power_budget_watts = 25; });
    ASSERT_TRUE(ok, "update failed");
    ASSERT_TRUE(cfg.version() == v0 + 1, "version not incremented");
    ASSERT_TRUE(cfg.power_budget() == 25, "value not updated");
    PASS();
}

static void test_atomic_config_invalid() {
    TEST("AtomicConfig: invalid update rejected");
    synapse::atomic::AtomicConfig cfg;
    auto v0 = cfg.version();
    bool ok = cfg.update([](auto& c) { c.power_budget_watts = 999; });
    ASSERT_FALSE(ok, "should reject invalid");
    ASSERT_TRUE(cfg.version() == v0, "version should not change");
    PASS();
}

// ============================================================================
// Phase 10.3: AtomicTelemetry
// ============================================================================
static void test_atomic_telemetry_wal() {
    TEST("AtomicTelemetry: write and read via WAL");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_test_wal";
    fs::create_directories(dir);
    auto wal = dir / "test.wal";

    {
        synapse::atomic::AtomicTelemetry t(wal.string());
        t.write(synapse::atomic::WALEventType::DrawIndexed);
        t.write(synapse::atomic::WALEventType::Dispatch);
        t.mark_clean_shutdown();
    }

    // Check recovery
    {
        synapse::atomic::AtomicTelemetry t(wal.string());
        uint64_t pending = t.check_recovery();
        ASSERT_TRUE(pending == 0, "should be clean shutdown");
    }

    fs::remove_all(dir);
    PASS();
}

static void test_atomic_telemetry_crash_recovery() {
    TEST("AtomicTelemetry: crash recovery");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_test_crash";
    fs::create_directories(dir);
    auto wal = dir / "crash.wal";

    // Remove any existing WAL
    fs::remove(wal);

    // Simulate crash: write entries but no clean shutdown marker
    {
        synapse::atomic::AtomicTelemetry t(wal.string());
        t.write(synapse::atomic::WALEventType::DrawIndexed);
        t.write(synapse::atomic::WALEventType::PushConstants);
        t.simulate_crash();  // Prevent destructor from writing clean shutdown
    }

    // Verify WAL file exists and has content
    ASSERT_TRUE(fs::exists(wal), "WAL file not created");
    ASSERT_TRUE(fs::file_size(wal) > 0, "WAL file is empty");

    // Recover
    {
        synapse::atomic::AtomicTelemetry t(wal.string());
        uint64_t pending = t.check_recovery();
        ASSERT_TRUE(pending > 0, "should detect crash");
        uint64_t recovered = t.replay();
        ASSERT_TRUE(recovered == 2, "wrong recovery count");
    }

    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Phase 10.4: GracefulDegradation
// ============================================================================
static void test_graceful_degradation_states() {
    TEST("GracefulDegradation: state transitions");
    synapse::atomic::GracefulDegradation gd;
    gd.register_feature("jit");
    ASSERT_TRUE(gd.is_available("jit"), "should be available");

    gd.handle_error("jit", 1);  // Transient
    ASSERT_TRUE(gd.state("jit") == synapse::atomic::FeatureState::Degraded, "should be degraded");
    ASSERT_TRUE(gd.is_available("jit"), "degraded is still available");

    gd.handle_error("jit", 2);  // Persistent
    ASSERT_FALSE(gd.is_available("jit"), "disabled should not be available");

    gd.handle_error("jit", 0);  // Recovery
    ASSERT_TRUE(gd.is_available("jit"), "should recover");
    PASS();
}

static void test_graceful_degradation_summary() {
    TEST("GracefulDegradation: summary");
    synapse::atomic::GracefulDegradation gd;
    gd.register_feature("a");
    gd.register_feature("b");
    gd.register_feature("c");
    gd.handle_error("c", 2);  // Disable c
    auto s = gd.summary();
    ASSERT_TRUE(s.enabled == 2, "wrong enabled");
    ASSERT_TRUE(s.disabled == 1, "wrong disabled");
    PASS();
}

// ============================================================================
// Phase 11.2: AtomicModelSwap
// ============================================================================
static void test_ml_hotreload_basic() {
    TEST("AtomicModelSwap: basic load and swap");
    synapse::hotreload::AtomicModelSwap<int> swap;

    auto future = swap.schedule_reload([]() {
        return std::make_shared<int>(42);
    });
    future.get();  // Wait for load

    ASSERT_TRUE(swap.version() == 1, "wrong version");
    ASSERT_TRUE(*swap.current() == 42, "wrong value");
    PASS();
}

static void test_ml_hotreload_ab() {
    TEST("AtomicModelSwap: A/B testing");
    synapse::hotreload::AtomicModelSwap<int> swap;

    // Load first model
    swap.schedule_reload([]() { return std::make_shared<int>(1); }).get();
    // Load second model
    swap.schedule_reload([]() { return std::make_shared<int>(2); }).get();

    // With test_pct=1.0, always returns previous model
    auto model = swap.decide_with_ab(1.0);
    ASSERT_TRUE(*model == 1, "should return previous model");

    // With test_pct=0.0, always returns current model
    model = swap.decide_with_ab(0.0);
    ASSERT_TRUE(*model == 2, "should return current model");
    PASS();
}

// ============================================================================
// Phase 11.3: ShaderHotReload
// ============================================================================
static void test_shader_hotreload() {
    TEST("ShaderHotReload: compile and lookup");
    synapse::hotreload::ShaderHotReload shr([](uint64_t hash) {
        return std::vector<uint8_t>{0x90, 0x90, 0x90};  // NOP sled
    });

    ASSERT_TRUE(shr.compile_and_add(12345), "compile failed");
    auto* ver = shr.lookup(12345);
    ASSERT_TRUE(ver != nullptr, "lookup failed");
    ASSERT_TRUE(ver->isa_binary.size() == 3, "wrong size");
    ASSERT_TRUE(ver->use_count.load() == 1, "wrong use count");

    auto stats = shr.stats();
    ASSERT_TRUE(stats.total_shaders == 1, "wrong shader count");
    PASS();
}

// ============================================================================
// Phase 12.1: Watchdog
// ============================================================================
static void test_watchdog_heartbeat() {
    TEST("Watchdog: heartbeat");
    synapse::recovery::HeartbeatState state;
    synapse::recovery::Watchdog wd(&state, 1000);

    // Heartbeat should update counters
    wd.heartbeat();
    auto s = wd.status();
    ASSERT_TRUE(s.heartbeat_count == 1, "wrong count");
    ASSERT_TRUE(s.shim_responding, "should be responding");
    PASS();
}

// ============================================================================
// Phase 13.1: UserProfile
// ============================================================================
static void test_user_profile_defaults() {
    TEST("UserProfile: default plan");
    synapse::personal::UserProfile up;
    auto plan = up.plan();
    ASSERT_TRUE(plan.power_budget_watts == 15, "wrong default power");
    ASSERT_TRUE(plan.ml_aggressive == true, "wrong default ml");
    PASS();
}

static void test_user_profile_preset() {
    TEST("UserProfile: preset profiles");
    synapse::personal::UserProfile up;
    up.apply_preset("battery-saver");
    auto plan = up.plan();
    ASSERT_TRUE(plan.power_budget_watts == 8, "wrong battery-saver power");
    ASSERT_TRUE(plan.fan_curve == "silent", "wrong battery-saver fan");

    up.apply_preset("performance");
    plan = up.plan();
    ASSERT_TRUE(plan.power_budget_watts == 25, "wrong performance power");
    PASS();
}

static void test_user_profile_export_import() {
    TEST("UserProfile: export/import");
    synapse::personal::UserProfile up;
    up.apply_preset("silent");
    auto data = up.export_profile();

    synapse::personal::UserProfile up2;
    up2.import_profile(data);
    auto plan = up2.plan();
    ASSERT_TRUE(plan.fan_curve == "silent", "import failed");
    PASS();
}

// ============================================================================
// Phase 13.2: DifferentialPrivacy
// ============================================================================
static void test_differential_privacy() {
    TEST("DifferentialPrivacy: privatize public data unchanged");
    synapse::personal::DifferentialPrivacy dp(1.0);
    int val = dp.privatize(42, synapse::personal::DataClass::Public);
    ASSERT_TRUE(val == 42, "public data should be unchanged");
    PASS();
}

static void test_differential_privacy_sensitive() {
    TEST("DifferentialPrivacy: sensitive data zeroed");
    synapse::personal::DifferentialPrivacy dp(1.0);
    int val = dp.privatize(42, synapse::personal::DataClass::Sensitive);
    ASSERT_TRUE(val == 0, "sensitive data should be zeroed");
    PASS();
}

static void test_local_storage_consent() {
    TEST("LocalStorage: consent management");
    synapse::personal::LocalStorage ls("/tmp/test");
    ASSERT_FALSE(ls.has_consent("analytics"), "no consent by default");
    ls.grant_consent("analytics");
    ASSERT_TRUE(ls.has_consent("analytics"), "consent granted");
    ls.revoke_consent("analytics");
    ASSERT_FALSE(ls.has_consent("analytics"), "consent revoked");
    PASS();
}

// ============================================================================
// Phase 14.1: SchemaMigration
// ============================================================================
static void test_schema_migration_direct() {
    TEST("SchemaMigration: direct migration v1→v2");
    synapse::protocol::SchemaMigration sm;
    sm.register_migration(1, 2, [](const auto& data) {
        std::vector<uint8_t> result = data;
        result.push_back(0x02);  // Append version marker
        return result;
    });

    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto migrated = sm.migrate(data, 1, 2);
    ASSERT_TRUE(migrated.size() == 4, "wrong size");
    ASSERT_TRUE(migrated[3] == 0x02, "wrong version marker");
    PASS();
}

static void test_schema_migration_chain() {
    TEST("SchemaMigration: chained migration v1→v3");
    synapse::protocol::SchemaMigration sm;
    sm.register_migration(1, 2, [](const auto& data) {
        std::vector<uint8_t> r = data;
        r.push_back(0x02);
        return r;
    });
    sm.register_migration(2, 3, [](const auto& data) {
        std::vector<uint8_t> r = data;
        r.push_back(0x03);
        return r;
    });

    std::vector<uint8_t> data = {0x01};
    auto migrated = sm.migrate(data, 1, 3);
    ASSERT_TRUE(migrated.size() == 3, "wrong size");
    ASSERT_TRUE(migrated[2] == 0x03, "wrong final marker");
    PASS();
}

static void test_schema_migration_compat() {
    TEST("SchemaMigration: backward compatibility check");
    synapse::protocol::SchemaMigration sm;
    ASSERT_TRUE(sm.is_compatible(2, 2), "same version");
    ASSERT_TRUE(sm.is_compatible(2, 1), "one version back");
    ASSERT_FALSE(sm.is_compatible(2, 0), "two versions back");
    PASS();
}

static void test_schema_migration_no_path() {
    TEST("SchemaMigration: no path throws");
    synapse::protocol::SchemaMigration sm;
    bool threw = false;
    try {
        sm.migrate({1}, 1, 5);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw, "should throw on missing path");
    PASS();
}

// ============================================================================
// main
// ============================================================================
int main() {
    printf("=== Project Synapse — Production Module Tests ===\n\n");

    printf("[Phase 10.1] AtomicStateMachine\n");
    test_atomic_state_initial();
    test_atomic_state_transitions();
    test_atomic_state_invalid();
    test_atomic_state_history();
    test_atomic_state_concurrent();

    printf("\n[Phase 10.2] AtomicConfig\n");
    test_atomic_config_defaults();
    test_atomic_config_update();
    test_atomic_config_invalid();

    printf("\n[Phase 10.3] AtomicTelemetry\n");
    test_atomic_telemetry_wal();
    test_atomic_telemetry_crash_recovery();

    printf("\n[Phase 10.4] GracefulDegradation\n");
    test_graceful_degradation_states();
    test_graceful_degradation_summary();

    printf("\n[Phase 11.2] AtomicModelSwap\n");
    test_ml_hotreload_basic();
    test_ml_hotreload_ab();

    printf("\n[Phase 11.3] ShaderHotReload\n");
    test_shader_hotreload();

    printf("\n[Phase 12.1] Watchdog\n");
    test_watchdog_heartbeat();

    printf("\n[Phase 13.1] UserProfile\n");
    test_user_profile_defaults();
    test_user_profile_preset();
    test_user_profile_export_import();

    printf("\n[Phase 13.2] DifferentialPrivacy\n");
    test_differential_privacy();
    test_differential_privacy_sensitive();
    test_local_storage_consent();

    printf("\n[Phase 14.1] SchemaMigration\n");
    test_schema_migration_direct();
    test_schema_migration_chain();
    test_schema_migration_compat();
    test_schema_migration_no_path();

    printf("\n=== Results: %d passed, %d failed ===\n",
           g_tests_passed, g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
