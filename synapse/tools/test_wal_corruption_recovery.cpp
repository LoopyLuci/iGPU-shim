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

static void write_raw_entry(const fs::path& path, const WALEntry& entry) {
    std::ofstream ofs(path, std::ios::app | std::ios::binary);
    assert(ofs && "failed to open WAL for raw append");
    ofs.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
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
    assert(replayed == 0 && "replay should recover no entries");
}

static void test_mid_file_junk() {
    auto dir = fs::temp_directory_path() / "synapse_wal_junk";
    fs::create_directories(dir);
    auto wal_path = dir / "junk.wal";
    fs::remove(wal_path);

    append_entry(wal_path, WALEventType::Dispatch);
    append_entry(wal_path, WALEventType::DrawIndexed);

    WALEntry junk{};
    junk.sequence = 42;
    junk.event_type = static_cast<WALEventType>(0xDEADBEEFu);
    write_raw_entry(wal_path, junk);
    append_entry(wal_path, WALEventType::CleanShutdown);

    synapse::atomic::AtomicTelemetry telemetry(wal_path.string());
    const uint64_t recovered = telemetry.check_recovery();
    const uint64_t replayed = telemetry.replay();

    fs::remove_all(dir);
    assert(recovered == 2 && "check_recovery should skip the junk entry");
    assert(replayed == 2 && "replay should skip the junk entry");
}

static void test_sequence_gap_detection() {
    auto dir = fs::temp_directory_path() / "synapse_wal_gap";
    fs::create_directories(dir);
    auto wal_path = dir / "gap.wal";
    fs::remove(wal_path);

    append_entry(wal_path, WALEventType::Dispatch);
    append_entry(wal_path, WALEventType::DrawIndexed);
    append_entry(wal_path, WALEventType::Dispatch);

    synapse::atomic::AtomicTelemetry telemetry(wal_path.string());
    auto entries = telemetry.read_wal();
    const uint64_t gaps = synapse::atomic::AtomicTelemetry::sequence_gap_count(entries);

    fs::remove_all(dir);
    assert(gaps == 0 && "contiguous writes should produce no sequence gaps");
}

static void test_sequence_gap_injected() {
    auto dir = fs::temp_directory_path() / "synapse_wal_gap_injected";
    fs::create_directories(dir);
    auto wal_path = dir / "gap_injected.wal";
    fs::remove(wal_path);

    append_entry(wal_path, WALEventType::Dispatch);
    append_entry(wal_path, WALEventType::DrawIndexed);

    WALEntry jump{};
    jump.sequence = 42;
    jump.event_type = WALEventType::Dispatch;
    write_raw_entry(wal_path, jump);
    append_entry(wal_path, WALEventType::CleanShutdown);

    synapse::atomic::AtomicTelemetry telemetry(wal_path.string());
    auto entries = telemetry.read_wal();
    const uint64_t gaps = synapse::atomic::AtomicTelemetry::sequence_gap_count(entries);

    fs::remove_all(dir);
    assert(gaps > 0 && "injected sequence jump should produce a gap");
}

static void test_partial_batch_recovery() {
    auto dir = fs::temp_directory_path() / "synapse_wal_partial_batch";
    fs::create_directories(dir);
    auto wal_path = dir / "partial_batch.wal";
    fs::remove(wal_path);

    append_entry(wal_path, WALEventType::Dispatch);
    append_entry(wal_path, WALEventType::DrawIndexed);
    append_entry(wal_path, WALEventType::Dispatch);

    // Overwrite the middle entry to simulate a partial-batch corruption.
    std::fstream fs(wal_path, std::ios::in | std::ios::out | std::ios::binary);
    assert(fs && "failed to open WAL for partial-batch corruption");
    fs.seekp(sizeof(WALEntry), std::ios::beg);
    WALEntry junk{};
    junk.sequence = 0xDEAD;
    junk.event_type = static_cast<WALEventType>(0xDEADBEEFu);
    fs.write(reinterpret_cast<const char*>(&junk), sizeof(junk));
    fs.close();

    synapse::atomic::AtomicTelemetry telemetry(wal_path.string());
    const uint64_t recovered = telemetry.check_recovery();
    const uint64_t replayed = telemetry.replay();

    fs::remove_all(dir);
    assert(recovered == 2 && "check_recovery should skip corrupted middle entry");
    assert(replayed == 2 && "replay should recover only valid entries");
}

static void test_batch_boundary_recovery() {
    auto dir = fs::temp_directory_path() / "synapse_wal_batch_boundary";
    fs::create_directories(dir);
    auto wal_path = dir / "batch_boundary.wal";
    fs::remove(wal_path);

    // Write exactly kWALFlushBatch entries to force a batch flush.
    for (size_t i = 0; i < synapse::atomic::AtomicTelemetry::kWALFlushBatch; ++i) {
        append_entry(wal_path, WALEventType::Dispatch);
    }

    // Corrupt the last entry in the first batch.
    std::fstream fs(wal_path, std::ios::in | std::ios::out | std::ios::binary);
    assert(fs && "failed to open WAL for batch-boundary corruption");
    const int64_t boundary_offset = static_cast<int64_t>(
        (synapse::atomic::AtomicTelemetry::kWALFlushBatch - 1) * sizeof(WALEntry));
    fs.seekp(boundary_offset, std::ios::beg);
    WALEntry junk{};
    junk.sequence = 0xDEAD;
    junk.event_type = static_cast<WALEventType>(0xDEADBEEFu);
    fs.write(reinterpret_cast<const char*>(&junk), sizeof(junk));
    fs.close();

    synapse::atomic::AtomicTelemetry telemetry(wal_path.string());
    const uint64_t recovered = telemetry.check_recovery();
    const uint64_t replayed = telemetry.replay();

    fs::remove_all(dir);
    assert(recovered == synapse::atomic::kWALFlushBatch - 1 &&
           "check_recovery should skip corrupted batch-boundary entry");
    assert(replayed == synapse::atomic::kWALFlushBatch - 1 &&
           "replay should recover only valid entries");
}

int main() {
    printf("=== WAL Corruption Recovery ===\n");

    test_truncated_tail();
    printf("  truncated tail: PASS\n");

    test_simulate_crash();
    printf("  simulated crash: PASS\n");

    test_empty_wal();
    printf("  empty WAL: PASS\n");

    test_mid_file_junk();
    printf("  mid-file junk: PASS\n");

    test_sequence_gap_detection();
    printf("  sequence-gap detection: PASS\n");

    test_sequence_gap_injected();
    printf("  sequence-gap injected: PASS\n");

    test_partial_batch_recovery();
    printf("  partial-batch recovery: PASS\n");

    test_batch_boundary_recovery();
    printf("  batch-boundary recovery: PASS\n");

    printf("Result: PASS\n");
    return 0;
}
