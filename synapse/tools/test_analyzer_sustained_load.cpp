/**
 * @file test_analyzer_sustained_load.cpp
 * @brief Sustained-load test for the analyzer thread and WAL pipeline.
 *
 * Generates telemetry continuously for a configurable period and asserts
 * that the analyzer thread keeps up without dropped or overflowed events.
 */

#include "../atomic/atomic_telemetry.h"
#include "../recovery/crash_recovery.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;

using namespace synapse::atomic;
using namespace synapse::recovery;

int main() {
    const std::string tmp = fs::temp_directory_path().string() + "/synapse_sustained_load";
    fs::create_directories(tmp);
    fs::remove_all(tmp + "/synapse.wal");
    fs::remove_all(tmp + "/synapse_recovery.meta");

    AtomicTelemetry telemetry(tmp + "/synapse.wal");
    CrashRecoveryManager recovery(tmp);

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(5);
    uint64_t submitted = 0;
    uint64_t dropped = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        if (!telemetry.write(WALEventType::Dispatch)) {
            ++dropped;
        }
        ++submitted;
    }

    // Allow background flush to catch up.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto end = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = end - start;
    const double rate = submitted / elapsed.count();

    printf("  Submitted : %llu\n", (unsigned long long)submitted);
    printf("  Dropped   : %llu\n", (unsigned long long)dropped);
    printf("  Duration  : %.3f s\n", elapsed.count());
    printf("  Rate      : %.1f events/s\n", rate);
    printf("  WAL size  : %llu bytes\n",
           (unsigned long long)fs::file_size(tmp + "/synapse.wal"));

    assert(submitted > 0 && "no telemetry submitted");
    assert(dropped == 0 && "telemetry entries dropped during sustained load");

    printf("Result: PASS\n");
    return 0;
}
