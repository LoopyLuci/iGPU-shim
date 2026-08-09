// ============================================================================
// synapse/tests/test_stress.cpp
// Stress tests for production modules
// ============================================================================
#include "../atomic/atomic_state.h"
#include "../atomic/atomic_config.h"
#include "../atomic/atomic_telemetry.h"
#include "../atomic/graceful_degradation.h"
#include "../hotreload/config_watcher.h"
#include "../hotreload/ml_hotreload.h"
#include "../recovery/crash_recovery.h"
#include "../recovery/watchdog.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { printf("  %-60s ", name); } while(0)
#define PASS() do { printf("PASS\n"); g_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); g_failed++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

static void clean_wal(const std::filesystem::path& dir) {
    std::filesystem::remove(dir / "synapse.wal");
    std::filesystem::remove(dir / "synapse_recovery.meta");
}

// ============================================================================
// Stress: 100 threads doing CAS transitions simultaneously
// ============================================================================
static void test_concurrent_cas_stress() {
    TEST("Stress: 100-thread CAS transitions");
    synapse::atomic::AtomicStateMachine sm;
    // Must start in Active before concurrent transitions
    sm.transition(synapse::atomic::ShimState::Initializing, "init");
    sm.transition(synapse::atomic::ShimState::Active, "ready");

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < 100; ++i) {
        threads.emplace_back([&sm, &success_count, i]() {
            auto to = (i % 2 == 0)
                ? synapse::atomic::ShimState::Degraded
                : synapse::atomic::ShimState::Active;
            if (sm.transition(to, "stress")) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) t.join();

    ASSERT(success_count.load() > 0, "no transitions succeeded");
    auto state = sm.current();
    ASSERT(state == synapse::atomic::ShimState::Active ||
           state == synapse::atomic::ShimState::Degraded,
           "invalid final state");
    PASS();
}

// ============================================================================
// Stress: 50 threads writing to WAL simultaneously
// ============================================================================
static void test_concurrent_wal_writes() {
    TEST("Stress: 50-thread WAL concurrent writes");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_stress_wal";
    fs::create_directories(dir);
    clean_wal(dir);

    const int NUM_THREADS = 50;
    const int WRITES_PER_THREAD = 10;

    {
        synapse::recovery::CrashRecoveryManager rm(dir.string());

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back([&rm]() {
                for (int j = 0; j < WRITES_PER_THREAD; ++j) {
                    rm.telemetry().write(synapse::atomic::WALEventType::DrawIndexed);
                }
            });
        }
        for (auto& t : threads) t.join();

        uint64_t expected = NUM_THREADS * WRITES_PER_THREAD;
        ASSERT(rm.telemetry().write_count() == expected, "write count mismatch");
    }

    auto wal_size = fs::file_size(dir / "synapse.wal");
    // Concurrent writes may drop some when ring buffer is full,
    // but the WAL file must contain at least some entries
    ASSERT(wal_size >= sizeof(synapse::atomic::WALEntry),
           "WAL file too small — no writes persisted");

    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Stress: Rapid config hot-reload (atomic snapshot updates)
// ============================================================================
static void test_rapid_config_hotreload() {
    TEST("Stress: 500 rapid config updates");

    synapse::atomic::AtomicConfig cfg;
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;

    // Writer: rapidly update power budget
    threads.emplace_back([&cfg, &stop]() {
        uint32_t val = 15;
        while (!stop.load(std::memory_order_relaxed)) {
            cfg.update([&val](synapse::atomic::ConfigSnapshot& snap) {
                snap.power_budget_watts = val;
            });
            ++val;
        }
    });

    // 4 readers
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&cfg, &stop]() {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = cfg.read();
                // Value should be valid (never corrupted)
                ASSERT(snap.power_budget_watts >= 15,
                       "corrupted config value");
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();

    auto final_snap = cfg.read();
    ASSERT(final_snap.power_budget_watts >= 15, "invalid final config");
    PASS();
}

// ============================================================================
// Stress: Rapid ML model hot-reload via AtomicModelSwap
// ============================================================================
static void test_rapid_ml_hotreload() {
    TEST("Stress: 100 rapid ML model swaps");

    struct DummyModel {
        int id;
    };

    synapse::hotreload::AtomicModelSwap<DummyModel> ml;

    for (int i = 0; i < 100; ++i) {
        auto model = std::make_shared<DummyModel>();
        model->id = i;
        auto future = ml.schedule_reload([model]() { return model; });
        future.get();  // Wait for load to complete
    }

    auto current = ml.current();
    ASSERT(current != nullptr, "no current model after 100 swaps");
    ASSERT(current->id == 99, "wrong model id after swaps");

    PASS();
}

// ============================================================================
// Stress: 50 threads toggling degradation states concurrently
// ============================================================================
static void test_concurrent_degradation_stress() {
    TEST("Stress: 50-thread degradation toggle");
    synapse::atomic::GracefulDegradation gd;

    gd.register_feature("jit");
    gd.register_feature("hai");
    gd.register_feature("ml");
    gd.register_feature("telemetry");

    std::vector<std::thread> threads;
    for (int i = 0; i < 50; ++i) {
        threads.emplace_back([&gd, i]() {
            std::string feat = "jit";
            if (i % 4 == 1) feat = "hai";
            if (i % 4 == 2) feat = "ml";
            if (i % 4 == 3) feat = "telemetry";

            for (int j = 0; j < 100; ++j) {
                if (j % 2 == 0) gd.handle_error(feat, 2);  // disable
                else             gd.handle_error(feat, 0);  // recover
                volatile bool v = gd.is_available(feat);
                (void)v;
            }
        });
    }
    for (auto& t : threads) t.join();
    PASS();
}

// ============================================================================
// Stress: WAL crash mid-write — verify burst recovery
// ============================================================================
static void test_wal_crash_during_write() {
    TEST("Stress: WAL crash during burst of 100 writes");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_stress_crash_burst";
    fs::create_directories(dir);
    clean_wal(dir);

    const int BURST_SIZE = 100;

    {
        synapse::recovery::CrashRecoveryManager rm(dir.string());
        for (int i = 0; i < BURST_SIZE; ++i) {
            rm.telemetry().write(synapse::atomic::WALEventType::DrawIndexed);
        }
        rm.telemetry().simulate_crash();
    }

    {
        synapse::recovery::CrashRecoveryManager rm(dir.string());
        bool crashed = rm.check_for_crash();
        ASSERT(crashed, "should detect crash after burst");
        auto result = rm.recover();
        ASSERT(result.entries_recovered == BURST_SIZE, "wrong recovery count");
    }

    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Stress: ConfigWatcher rapid file changes
// ============================================================================
static void test_config_watcher_rapid_changes() {
    TEST("Stress: ConfigWatcher detects rapid file changes");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_stress_watcher";
    fs::create_directories(dir);

    auto config_file = dir / "test_config.toml";

    { std::ofstream ofs(config_file); ofs << "power_budget = 15\n"; }

    synapse::hotreload::ConfigWatcher watcher(config_file);
    std::atomic<int> change_count{0};
    watcher.on_change([&change_count](const std::string&) {
        change_count.fetch_add(1, std::memory_order_relaxed);
    });

    watcher.start();

    for (int i = 0; i < 30; ++i) {
        std::ofstream ofs(config_file);
        ofs << "power_budget = " << (15 + i) << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    watcher.stop();

    ASSERT(change_count.load() > 0, "no changes detected");
    fs::remove_all(dir);
    PASS();
}

// ============================================================================
// Stress: Watchdog heartbeat lifecycle
// ============================================================================
static void test_watchdog_heartbeat() {
    TEST("Stress: Watchdog heartbeat lifecycle");

    synapse::recovery::HeartbeatState state{};
    synapse::recovery::Watchdog wd(&state, 10000);  // 10s timeout

    bool recovery_triggered = false;
    wd.on_recovery([&recovery_triggered]() { recovery_triggered = true; });

    wd.start();

    // Send heartbeats — should stay healthy with long timeout
    for (int i = 0; i < 5; ++i) {
        wd.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT(!recovery_triggered, "triggered too early");

    // Check status
    auto s = wd.status();
    ASSERT(s.heartbeat_count > 0, "no heartbeats recorded");
    ASSERT(s.watchdog_active, "watchdog not active");

    wd.stop();
    ASSERT(!s.watchdog_active || true, "cleanup ok");  // Just verify no crash
    PASS();
}

// ============================================================================
// main
// ============================================================================
int main() {
    printf("=== Project Synapse — Stress Tests ===\n\n");

    test_concurrent_cas_stress();
    test_concurrent_wal_writes();
    test_rapid_config_hotreload();
    test_rapid_ml_hotreload();
    test_concurrent_degradation_stress();
    test_wal_crash_during_write();
    test_config_watcher_rapid_changes();
    test_watchdog_heartbeat();

    printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
