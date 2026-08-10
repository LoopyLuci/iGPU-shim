// ============================================================================
// synapse/tools/synapse_cli.cpp
// CLI entry point for Synapse diagnostics, ML, and benchmarking.
//
// Usage:
//   synapse_cli snapshot                          — dump session snapshot
//   synapse_cli coverage                          — show coverage summary
//   synapse_cli train checkpoint [path]           — save ML checkpoint
//   synapse_cli train restore [path]              — restore ML checkpoint
//   synapse_cli explain [action]                  — show ML weights for action
//   synapse_cli run-scenario                      — run a trivial scenario
//   synapse_cli benchmark [--iterations N]        — hot-path overhead benchmark
//   synapse_cli benchmark --help                  — show benchmark help
// ============================================================================

#include <iostream>
#include <string>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <cstring>

#include "../testing/agent_api.h"
#include "../testing/scenario_runner.h"
#include "../ml/ml_sub_api.h"
#include "../atomic/atomic_config.h"
#include "../atomic/graceful_degradation.h"
#include "../atomic/atomic_telemetry.h"
#include "../recovery/crash_recovery.h"

// ============================================================================
// Benchmark helpers
// ============================================================================
#define BENCH_START(name) auto _bench_start_##name = std::chrono::high_resolution_clock::now()
#define BENCH_END_MS(name) \
    std::chrono::duration<double, std::milli>( \
        std::chrono::high_resolution_clock::now() - _bench_start_##name).count()

struct BenchResult {
    const char* name;
    double total_ms;
    double per_op_ns_or_us;
    bool passed;
};

static std::vector<BenchResult> run_benchmark(int iterations) {
    std::vector<BenchResult> results;
    namespace fs = std::filesystem;

    // --- 1. AtomicConfig::read() ---
    {
        synapse::atomic::AtomicConfig config;
        for (int i = 0; i < 1000; i++) config.read();

        BENCH_START(t);
        for (int i = 0; i < iterations; i++) {
            volatile auto snap = config.read();
            (void)snap;
        }
        double ms = BENCH_END_MS(t);
        double ns = (ms * 1e6) / iterations;
        bool ok = ns < 1000.0;
        printf("  AtomicConfig::read()          %8d ops in %7.1f ms  → %7.1f ns/op   %s\n",
               iterations, ms, ns, ok ? "PASS" : "FAIL");
        results.push_back({"config_read", ms, ns, ok});
    }

    // --- 2. GracefulDegradation::is_available() ---
    {
        synapse::atomic::GracefulDegradation degrade;
        degrade.register_feature("jit", synapse::atomic::FeatureState::Enabled);
        for (int i = 0; i < 1000; i++) degrade.is_available("jit");

        BENCH_START(t);
        for (int i = 0; i < iterations; i++) {
            volatile bool v = degrade.is_available("jit");
            (void)v;
        }
        double ms = BENCH_END_MS(t);
        double ns = (ms * 1e6) / iterations;
        bool ok = ns < 1000.0;
        printf("  Degradation::is_available()   %8d ops in %7.1f ms  → %7.1f ns/op   %s\n",
               iterations, ms, ns, ok ? "PASS" : "FAIL");
        results.push_back({"degrade_check", ms, ns, ok});
    }

    // --- 3. WAL write throughput ---
    {
        auto dir = fs::temp_directory_path() / "synapse_cli_bench_wal";
        fs::create_directories(dir);
        auto wal_path = dir / "bench.wal";
        fs::remove(wal_path);

        synapse::atomic::AtomicTelemetry tel(wal_path.string());
        for (int i = 0; i < 100; i++) {
            tel.write(synapse::atomic::WALEventType::DrawIndexed);
        }

        BENCH_START(t);
        for (int i = 0; i < iterations; i++) {
            tel.write(synapse::atomic::WALEventType::DrawIndexed);
        }
        double ms = BENCH_END_MS(t);
        double us = (ms * 1000.0) / iterations;
        double ops_sec = (iterations / ms) * 1000.0;
        bool ok = us < 50.0;
        printf("  WAL write                    %8d ops in %7.1f ms  → %7.1f μs/op   %.0f ops/sec  %s\n",
               iterations, ms, us, ops_sec, ok ? "PASS" : "FAIL");
        results.push_back({"wal_write", ms, us, ok});

        tel.simulate_crash();
        fs::remove_all(dir);
    }

    // --- 4. Combined hot-path (config + degrade + WAL) ---
    {
        auto dir = fs::temp_directory_path() / "synapse_cli_bench_combined";
        fs::create_directories(dir);
        auto wal_path = dir / "combined.wal";
        fs::remove(wal_path);

        synapse::atomic::AtomicConfig config;
        synapse::atomic::GracefulDegradation degrade;
        synapse::atomic::AtomicTelemetry tel(wal_path.string());
        degrade.register_feature("jit", synapse::atomic::FeatureState::Enabled);

        BENCH_START(t);
        for (int i = 0; i < iterations; i++) {
            volatile bool available = degrade.is_available("jit");
            (void)available;
            volatile auto snap = config.read();
            (void)snap;
            tel.write(synapse::atomic::WALEventType::DrawIndexed);
        }
        double ms = BENCH_END_MS(t);
        double us = (ms * 1000.0) / iterations;
        double frame_pct = (us / 16667.0) * 100.0;
        bool ok = us < 100.0;
        printf("  Combined hot-path            %8d ops in %7.1f ms  → %7.1f μs/op   %.2f%% frame budget  %s\n",
               iterations, ms, us, frame_pct, ok ? "PASS" : "FAIL");
        results.push_back({"combined_hotpath", ms, us, ok});

        tel.simulate_crash();
        fs::remove_all(dir);
    }

    // --- 5. Config update latency ---
    {
        synapse::atomic::AtomicConfig config;
        BENCH_START(t);
        for (int i = 0; i < iterations; i++) {
            config.update([i](synapse::atomic::ConfigSnapshot& snap) {
                snap.power_budget_watts = 15 + (i % 10);
            });
        }
        double ms = BENCH_END_MS(t);
        double us = (ms * 1000.0) / iterations;
        bool ok = us < 10.0;
        printf("  Config::update()             %8d ops in %7.1f ms  → %7.1f μs/op   %s\n",
               iterations, ms, us, ok ? "PASS" : "FAIL");
        results.push_back({"config_update", ms, us, ok});
    }

    return results;
}

static void print_benchmark_help() {
    printf("Usage: synapse_cli benchmark [--iterations N]\n\n");
    printf("Measures hot-path overhead for the production module integration:\n");
    printf("  - AtomicConfig::read()        (lock-free snapshot)\n");
    printf("  - GracefulDegradation check   (feature availability)\n");
    printf("  - WAL write throughput         (batched disk writes)\n");
    printf("  - Combined hot-path cycle      (config + degrade + WAL)\n");
    printf("  - AtomicConfig::update()       (lock-free CAS update)\n\n");
    printf("Options:\n");
    printf("  --iterations N    Number of iterations (default: 10000)\n");
    printf("  --help            Show this help message\n\n");
    printf("Output columns: ops, total time, per-op latency, throughput, pass/fail.\n");
    printf("The 'Combined hot-path' row shows %% of 16,667 μs frame budget at 60 FPS.\n");
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
    using namespace synapse::testing;
    using namespace synapse::ml;

    if (argc < 2) {
        std::cout << "synapse_cli: snapshot | inject | run-scenario | coverage | train | explain | benchmark\n";
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "benchmark") {
        int iterations = 10000;
        for (int i = 2; i < argc; i++) {
            if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
                print_benchmark_help();
                return 0;
            }
            if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
                iterations = std::atoi(argv[i + 1]);
                i++;
            }
        }

        printf("=== Synapse Hot-Path Benchmark (iterations=%d) ===\n\n", iterations);
        auto results = run_benchmark(iterations);

        int passed = 0, failed = 0;
        for (auto& r : results) {
            if (r.passed) passed++; else failed++;
        }
        printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
        return failed > 0 ? 1 : 0;

    } else if (cmd == "snapshot") {
        AgentAPI api;
        auto s = api.snapshot();
        std::cout << "Snapshot: (report heuristic) " << s.report.heuristic << "\n";

    } else if (cmd == "coverage") {
        std::cout << "Coverage: experimental (use test harness for full run)\n";

    } else if (cmd == "train") {
        MLSubAPI ml;
        if (argc >= 3 && std::string(argv[2]) == "checkpoint") {
            const std::string path = (argc>=4) ? argv[3] : "bandit.chk";
            ml.checkpoint(path);
            std::cout << "ML checkpoint saved to " << path << "\n";
        } else if (argc >= 3 && std::string(argv[2]) == "restore") {
            const std::string path = (argc>=4) ? argv[3] : "bandit.chk";
            ml.restore(path);
            std::cout << "ML restored from " << path << "\n";
        } else {
            std::cout << "train subcommands: checkpoint <path> | restore <path>\n";
        }

    } else if (cmd == "explain") {
        MLSubAPI ml;
        int action = (argc>=3) ? std::atoi(argv[2]) : 0;
        auto w = ml.explain_action(action);
        std::cout << "Weights for action " << action << ": ";
        for (float v : w) std::cout << v << " ";
        std::cout << "\n";

    } else if (cmd == "run-scenario") {
        AgentAPI api;
        ScenarioRunner runner(api);
        synapse::testing::TestScenario s; s.name = "cli-mini";
        synapse::WorkloadSignature sig{}; sig.vertex_count = 1000; sig.draw_call_count = 1;
        synapse::testing::TestStep step{sig, 10};
        s.steps.push_back(step);
        auto res = runner.run(s);
        std::cout << "Scenario " << res.scenario_name << " completed. passed=" << res.passed << "\n";

    } else {
        std::cout << "Unknown command. Available: snapshot inject run-scenario coverage train explain benchmark\n";
        return 1;
    }

    return 0;
}
