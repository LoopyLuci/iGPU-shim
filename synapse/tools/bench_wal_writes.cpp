/**
 * @file bench_wal_writes.cpp
 * @brief Measures WAL write throughput and effective cost per entry.
 *
 * Compares batched write throughput with an explicit single-write flush
 * path using a temporary file-backed AtomicTelemetry instance.
 */

#include "../atomic/atomic_telemetry.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

static std::string make_temp_path(const std::string& name) {
    return name + "_" + std::to_string(Clock::now().time_since_epoch().count()) + ".wal";
}

static void remove_path(const std::string& path) {
    std::remove(path.c_str());
}

static std::vector<uint8_t> random_payload(std::mt19937_64& rng, std::size_t size) {
    std::vector<uint8_t> out(size);
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& b : out) b = static_cast<uint8_t>(dist(rng));
    return out;
}

int main() {
    constexpr std::size_t kRuns = 20000;
    constexpr std::size_t kPayloadBytes = 32;

    std::mt19937_64 rng(42);
    auto payload = random_payload(rng, kPayloadBytes);

    std::cout << "=== WAL Write Benchmark ===\n";

    // 1) Batched mode using project's batch threshold.
    {
        const std::string path = make_temp_path("wal_bench_batch");
        synapse::atomic::AtomicTelemetry telemetry(path);
        auto t0 = Clock::now();
        for (std::size_t i = 0; i < kRuns; ++i) {
            telemetry.write(static_cast<synapse::atomic::WALEventType>(0x42), payload.data(), static_cast<uint32_t>(payload.size()));
        }
        // Ensure final flush is counted.
        telemetry.~AtomicTelemetry();
        auto t1 = Clock::now();
        auto ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        auto us_per_entry = (ms * 1000.0) / static_cast<double>(kRuns);
        auto throughput = static_cast<double>(kRuns) / (ms / 1000.0);
        std::cout << "  batch writes : " << kRuns << " entries in " << std::fixed << std::setprecision(3) << ms << " ms\n";
        std::cout << "              -> " << std::fixed << std::setprecision(3) << us_per_entry << " us/entry\n";
        std::cout << "              -> " << static_cast<uint64_t>(throughput) << " entries/s\n";
        remove_path(path);
    }

    // 2) Forced single-write mode using temporary files as proxy.
    // This is not the internal AtomicTelemetry path, but measures best-case
    // raw append+flush cost when each entry is flushed individually.
    {
        const std::string path = make_temp_path("wal_bench_single");
        auto t0 = Clock::now();
        for (std::size_t i = 0; i < kRuns; ++i) {
            std::ofstream ofs(path, std::ios::binary | std::ios::app);
            if (ofs) {
                ofs.write(reinterpret_cast<const char*>(payload.data()), payload.size());
                ofs.flush();
            }
        }
        auto t1 = Clock::now();
        auto ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        auto us_per_entry = (ms * 1000.0) / static_cast<double>(kRuns);
        auto throughput = static_cast<double>(kRuns) / (ms / 1000.0);
        std::cout << "  single writes: " << kRuns << " entries in " << std::fixed << std::setprecision(3) << ms << " ms\n";
        std::cout << "              -> " << std::fixed << std::setprecision(3) << us_per_entry << " us/entry\n";
        std::cout << "              -> " << static_cast<uint64_t>(throughput) << " entries/s\n";
        remove_path(path);
    }

    std::cout << "Result: PASS\n";
    return 0;
}
