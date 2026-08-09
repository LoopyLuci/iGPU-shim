// ============================================================================
// synapse/tests/test_hotpath_benchmark.cpp
// Hot-path overhead benchmark: measures cost of production module
// integration on the Vulkan draw-call hot path.
//
// Tests:
// 1. AtomicConfig::read() latency (lock-free snapshot)
// 2. GracefulDegradation::is_available() latency
// 3. WAL write throughput (entries/sec)
// 4. Combined overhead estimate for one handle_draw_indexed cycle
// ============================================================================

#include "../synapse_core.h"
#include "../atomic/atomic_config.h"
#include "../atomic/graceful_degradation.h"
#include "../atomic/atomic_telemetry.h"
#include "../recovery/crash_recovery.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failed++; \
    } else { \
        g_passed++; \
    } \
} while(0)

#define BENCH_START(name) auto _bench_start_##name = std::chrono::high_resolution_clock::now()
#define BENCH_END_MS(name) \
    std::chrono::duration<double, std::milli>( \
        std::chrono::high_resolution_clock::now() - _bench_start_##name).count()

// ---- Benchmark 1: AtomicConfig::read() latency ----
static void bench_config_read() {
    printf("[bench] AtomicConfig::read() latency\n");
    synapse::atomic::AtomicConfig config;

    // Warm up
    for (int i = 0; i < 1000; i++) config.read();

    const int N = 100000;
    BENCH_START(config_read);
    for (int i = 0; i < N; i++) {
        volatile auto snap = config.read();
        (void)snap;
    }
    double total_ms = BENCH_END_MS(config_read);
    double per_read_ns = (total_ms * 1000000.0) / N;

    printf("  %d reads in %.3f ms → %.1f ns/read\n", N, total_ms, per_read_ns);
    ASSERT(per_read_ns < 1000.0, "config.read() should be < 1μs (lock-free)");
}

// ---- Benchmark 2: GracefulDegradation::is_available() latency ----
static void bench_degradation_check() {
    printf("[bench] GracefulDegradation::is_available() latency\n");
    synapse::atomic::GracefulDegradation degrade;
    degrade.register_feature("jit", synapse::atomic::FeatureState::Enabled);
    degrade.register_feature("hai", synapse::atomic::FeatureState::Enabled);

    // Warm up
    for (int i = 0; i < 1000; i++) degrade.is_available("jit");

    const int N = 100000;
    BENCH_START(degrade_check);
    for (int i = 0; i < N; i++) {
        volatile bool v = degrade.is_available("jit");
        (void)v;
    }
    double total_ms = BENCH_END_MS(degrade_check);
    double per_check_ns = (total_ms * 1000000.0) / N;

    printf("  %d checks in %.3f ms → %.1f ns/check\n", N, total_ms, per_check_ns);
    ASSERT(per_check_ns < 1000.0, "is_available() should be < 1μs");
}

// ---- Benchmark 3: WAL write throughput ----
static void bench_wal_write() {
    printf("[bench] WAL write throughput\n");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_bench_wal";
    fs::create_directories(dir);
    auto wal_path = dir / "bench.wal";

    // Clean
    fs::remove(wal_path);

    synapse::atomic::AtomicTelemetry tel(wal_path.string());

    // Warm up
    for (int i = 0; i < 100; i++) {
        tel.write(synapse::atomic::WALEventType::DrawIndexed);
    }

    const int N = 10000;
    BENCH_START(wal_write);
    for (int i = 0; i < N; i++) {
        tel.write(synapse::atomic::WALEventType::DrawIndexed);
    }
    double total_ms = BENCH_END_MS(wal_write);

    double per_write_us = (total_ms * 1000.0) / N;
    double writes_per_sec = (N / total_ms) * 1000.0;

    printf("  %d writes in %.1f ms → %.1f μs/write, %.0f writes/sec\n",
           N, total_ms, per_write_us, writes_per_sec);
    ASSERT(per_write_us < 50.0, "WAL write should be < 50μs");

    // Cleanup
    tel.simulate_crash();  // Skip clean shutdown marker
    fs::remove_all(dir);
}

// ---- Benchmark 4: Combined hot-path simulation ----
static void bench_combined_hotpath() {
    printf("[bench] Combined hot-path overhead (config + degrade + WAL)\n");
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "synapse_bench_combined";
    fs::create_directories(dir);
    auto wal_path = dir / "combined.wal";
    fs::remove(wal_path);

    synapse::atomic::AtomicConfig config;
    synapse::atomic::GracefulDegradation degrade;
    synapse::atomic::AtomicTelemetry tel(wal_path.string());
    degrade.register_feature("jit", synapse::atomic::FeatureState::Enabled);

    // Simulate what handle_draw_indexed does per call:
    // 1. degrade_.is_available("jit")
    // 2. config_.read()
    // 3. wal_log(WALEventType::DrawIndexed)
    const int N = 10000;
    BENCH_START(combined);
    for (int i = 0; i < N; i++) {
        volatile bool available = degrade.is_available("jit");
        (void)available;
        volatile auto snap = config.read();
        (void)snap;
        tel.write(synapse::atomic::WALEventType::DrawIndexed);
    }
    double total_ms = BENCH_END_MS(combined);

    double per_call_us = (total_ms * 1000.0) / N;
    double calls_per_sec = (N / total_ms) * 1000.0;

    printf("  %d cycles in %.1f ms → %.1f μs/call, %.0f calls/sec\n",
           N, total_ms, per_call_us, calls_per_sec);
    printf("  At 60 FPS: %.2f μs overhead per frame (if 1 draw call)\n", per_call_us);
    printf("  Budget: 16,667 μs per frame → overhead is %.4f%%\n",
           (per_call_us / 16667.0) * 100.0);
    ASSERT(per_call_us < 100.0, "Combined hot-path should be < 100μs");

    // Cleanup
    tel.simulate_crash();
    fs::remove_all(dir);
}

// ---- Benchmark 5: Config update latency ----
static void bench_config_update() {
    printf("[bench] AtomicConfig::update() latency\n");
    synapse::atomic::AtomicConfig config;

    const int N = 10000;
    BENCH_START(config_update);
    for (int i = 0; i < N; i++) {
        config.update([i](synapse::atomic::ConfigSnapshot& snap) {
            snap.power_budget_watts = 15 + (i % 10);
        });
    }
    double total_ms = BENCH_END_MS(config_update);
    double per_update_us = (total_ms * 1000.0) / N;

    printf("  %d updates in %.1f ms → %.1f μs/update\n", N, total_ms, per_update_us);
    ASSERT(per_update_us < 10.0, "config.update() should be < 10μs");
}

int main() {
    printf("=== Hot-Path Overhead Benchmark ===\n\n");

    bench_config_read();
    bench_degradation_check();
    bench_wal_write();
    bench_combined_hotpath();
    bench_config_update();

    printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
