/**
 * @file test_wal_corruption_recovery.cpp
 * @brief WAL corruption recovery tests.
 *
 * Covers:
 *  - truncated final record
 *  - recovery after simulate_crash()
 *  - empty WAL behavior
 */

#include "../atomic/atomic_telemetry.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
using synapse::atomic::WALEntry;
using synapse::atomic::WALEventType;

static void append_entry(const fs::path& path, WALEventType type) {
    synapse::atomic::AtomicTelemetry telemetry(path.string());
    telemetry.write(type);
}

static void test_truncated_tail() {
    auto dir = fs::temp_directory_path() / "synapse_wal_corrupt_truncated";
    fs::create_directories(dir);
    auto wal_path = dir / "corrupt.wal";
    fs::remove(wal_path);

    append_entry(wal_path, WALEventType::Dispatch);
    append_entry(wal_path, WALEventType::DrawIndexed);

    // Truncate file to mid-second-entry.
    std::fstream fs(wal_path, std::ios::in | std::ios::out | std::ios::binary);
    assert(fs && "failed to open WAL for truncation");
    fs.seekp(sizeof(WALEntry) / 2, std::ios::beg);
    WALEntry zero{};
    fs.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
    fs.close();

    synapse::atomic::AtomicTelemetry telemetry(wal_path.string());
    const uint64_t recovered = telemetry.check_recovery();
    const uint64_t replayed = telemetry.replay();

    fs::remove_all(dir);
    assert(recovered == 1 && "check_recovery should report only the readable entry");
    assert(replayed == 1 && "replay should only replay readable entries");
}

static void test_simulate_crash() {
    auto dir = fs::temp_directory_path() / "synapse_wal_crash";
    fs::create_directories(dir);
    auto wal_path = dir / "crash.wal";
    fs::remove(wal_path);

    {
        synapse::atomic::AtomicTelemetry telemetry(wal_path.string());
        telemetry.write(WALEventType::Dispatch);
        telemetry.write(WALEventType::DrawIndexed);
        telemetry.simulate_crash();
    }

    synapse::atomic::AtomicTelemetry telemetry(wal_path.string());
    const uint64_t recovered = telemetry.check_recovery();
    const uint64_t replayed = telemetry.replay();

    fs::remove_all(dir);
    assert(!telemetry.is_clean_shutdown() && "simulated crash should leave WAL unclean");
    assert(recovered == 2 && "recovery should see both entries");
    assert(replayed == 2 && "replay should recover both entries");
}

static void test_empty_wal() {
    auto dir = fs::temp_directory_path() / "synapse_wal_empty";
    fs::create_directories(dir);
    auto wal_path = dir / "empty.wal";
    fs::remove(wal_path);

    std::ofstream(wal_path).close();

    synapse::atomic::AtomicTelemetry telemetry(wal_path.string());
    const uint64_t recovered = telemetry.check_recovery();
    const uint64_t replayed = telemetry.replay();

    fs::remove_all(dir);
    assert(recovered == 0 && "empty WAL should recover no entries");
    assert(replayed == 0 && "empty WAL should replay no entries");
}

int main() {
    printf("=== WAL Corruption Recovery ===\n");

    test_truncated_tail();
    printf("  truncated tail: PASS\n");

    test_simulate_crash();
    printf("  simulated crash: PASS\n");

    test_empty_wal();
    printf("  empty WAL: PASS\n");

    printf("Result: PASS\n");
    return 0;
}
