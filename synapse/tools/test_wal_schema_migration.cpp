/**@file test_wal_schema_migration.cpp
 * @brief WAL↔SchemaMigration integration — legacy v0 recovery metadata is
 *        migrated to v1 by CrashRecoveryManager on first load, and WAL
 *        entries are replayed through the migrated manager.
 *
 * Covers the integration point not exercised by the existing unit/integration
 * tests: a single CrashRecoveryManager open migrates a raw v0 .meta file
 * (no SYNA header) to v1 AND replays WAL entries that carry schema_version=1
 * in their payload. Verifies the on-disk .meta is rewritten as v1 and that
 * recovery metadata tracks recovered counts.
 */

#include "../recovery/crash_recovery.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using namespace synapse::recovery;
using namespace synapse::atomic;
using namespace synapse::protocol;

// ── Helpers ──────────────────────────────────────────────────────────────────

static void write_legacy_metadata(const std::string& path,
                                  const RecoveryMetadata& meta) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    assert(ofs && "cannot open .meta for writing");
    ofs.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
}

static bool read_file_bytes(const std::string& path,
                            std::vector<uint8_t>& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    out.assign(std::istreambuf_iterator<char>(ifs),
               std::istreambuf_iterator<char>());
    return true;
}

static bool is_v1_metadata_file(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!read_file_bytes(path, buf)) return false;
    if (buf.size() < sizeof(MessageHeader)) return false;
    MessageHeader hdr{};
    std::memcpy(&hdr, buf.data(), sizeof(hdr));
    return hdr.magic == 0x53594E41 && hdr.schema_version > 0;
}

static void assert_eq_metadata(const RecoveryMetadata& a,
                               const RecoveryMetadata& b) {
    assert(a.last_clean_shutdown_sequence == b.last_clean_shutdown_sequence);
    assert(a.last_wal_sequence == b.last_wal_sequence);
    assert(a.total_recoveries == b.total_recoveries);
    assert(a.total_entries_recovered == b.total_entries_recovered);
}

// ── Tests ────────────────────────────────────────────────────────────────────

int main() {
    printf("=== WAL → SchemaMigration Integration Test ===\n");

    const std::string tmp =
        fs::temp_directory_path().string() +
        "/synapse_wal_schema_migration";
    fs::create_directories(tmp);
    const std::string meta_path = tmp + "/synapse_recovery.meta";
    const std::string wal_path  = tmp + "/synapse.wal";
    fs::remove_all(meta_path);
    fs::remove_all(wal_path);

    // ── 1) Write legacy v0 metadata (raw struct, no header) ────────────────
    RecoveryMetadata legacy{};
    legacy.last_clean_shutdown_sequence = 7;
    legacy.last_wal_sequence            = 14;
    legacy.total_recoveries             = 3;
    legacy.total_entries_recovered      = 42;
    write_legacy_metadata(meta_path, legacy);
    printf("  Wrote legacy v0 metadata: seq=%llu, wal_seq=%llu, "
           "recoveries=%llu, recovered=%llu\n",
           static_cast<unsigned long long>(legacy.last_clean_shutdown_sequence),
           static_cast<unsigned long long>(legacy.last_wal_sequence),
           static_cast<unsigned long long>(legacy.total_recoveries),
           static_cast<unsigned long long>(legacy.total_entries_recovered));
    assert(!is_v1_metadata_file(meta_path) &&
           "file should NOT be v1 yet (no SYNA header)");

    // Also write WAL entries with schema_version=1 in payload.
    // No CleanShutdown marker → unclean → recovery needed.
    {
        std::ofstream wal(wal_path, std::ios::binary | std::ios::trunc);
        assert(wal);
        for (uint64_t i = 0; i < 3; ++i) {
            WALEntry e{};
            e.sequence     = i + 1;
            e.timestamp_ns = 0;
            e.event_type   = (i % 2 == 0) ? WALEventType::DrawIndexed
                                          : WALEventType::Dispatch;
            e.data_size    = 0;
            e.set_schema_version(kWALSchemaVersion);  // v1 in payload
            wal.write(reinterpret_cast<const char*>(&e), sizeof(e));
        }
    }
    printf("  Wrote 3 WAL entries with schema_version=%u (no CleanShutdown).\n",
           kWALSchemaVersion);

    // ── 2) Open CrashRecoveryManager — migrates v0 → v1 + detects crash ────
    CrashRecoveryManager mgr(tmp);
    bool crash_detected = mgr.check_for_crash();
    printf("  check_for_crash = %s (expected: true)\n",
           crash_detected ? "true" : "false");
    assert(crash_detected && "crash should be detected (no clean shutdown)");

    // ── 3) Verify on-disk metadata is now v1 ────────────────────────────────
    assert(is_v1_metadata_file(meta_path) &&
           "after load, .meta must be rewritten as v1 (SYNA header)");
    printf("  On-disk .meta is now v1 (SYNA header + schema_version=1).\n");

    // ── 4) Verify in-memory metadata matches original legacy values ─────────
    auto meta = mgr.metadata();
    assert_eq_metadata(meta, legacy);
    printf("  In-memory metadata matches legacy values after migration.\n");

    // ── 5) Verify schema path is registered and discoverable ────────────────
    assert(mgr.schema().has_path(0, kRecoverySchemaVersion) &&
           "v0 → v1 path must exist");
    assert(!mgr.schema().has_path(kRecoverySchemaVersion, 0) &&
           "v1 → v0 must not exist (no reverse migration)");
    printf("  Schema: v0→v1=%s, v1→v0=%s\n",
           mgr.schema().has_path(0, kRecoverySchemaVersion) ? "yes" : "no",
           mgr.schema().has_path(kRecoverySchemaVersion, 0) ? "yes" : "no");

    // ── 6) Recover — replay WAL entries through the migrated manager ────────
    auto result = mgr.recover();
    assert(result.success && "recovery should succeed");
    assert(result.entries_recovered == 3 &&
           "all 3 WAL entries should be recovered");
    printf("  Recovered %llu entries (expected 3).\n",
           static_cast<unsigned long long>(result.entries_recovered));

    // ── 7) Metadata updated after recovery ──────────────────────────────────
    auto meta2 = mgr.metadata();
    assert(meta2.total_recoveries == 1 &&
           "total_recoveries should be 1 after one recover() call");
    assert(meta2.total_entries_recovered == 3 &&
           "total_entries_recovered should track recovered count");
    assert(meta2.last_wal_sequence == 3 &&
           "last_wal_sequence should reflect replayed WAL");
    printf("  Post-recovery: total_recoveries=%llu, "
           "total_entries_recovered=%llu, last_wal_seq=%llu\n",
           static_cast<unsigned long long>(meta2.total_recoveries),
           static_cast<unsigned long long>(meta2.total_entries_recovered),
           static_cast<unsigned long long>(meta2.last_wal_sequence));

    // ── 8) Second legacy file — repeat the migration ────────────────────────
    fs::remove_all(meta_path);
    RecoveryMetadata legacy2{};
    legacy2.last_clean_shutdown_sequence = 0;
    legacy2.last_wal_sequence            = 100;
    legacy2.total_recoveries             = 5;
    legacy2.total_entries_recovered      = 200;
    write_legacy_metadata(meta_path, legacy2);

    {
        CrashRecoveryManager mgr2(tmp);
        bool crash2 = mgr2.check_for_crash();
        assert(crash2 && "second manager should also detect crash");
        auto m2 = mgr2.metadata();
        assert_eq_metadata(m2, legacy2);
        assert(is_v1_metadata_file(meta_path) &&
               "second .meta file should also be migrated to v1");
        printf("  Second legacy file: v0→v1 migration verified "
               "(wal_seq=%llu, recoveries=%llu).\n",
               static_cast<unsigned long long>(m2.last_wal_sequence),
               static_cast<unsigned long long>(m2.total_recoveries));
    }

    // ── 9) Clean shutdown path — no migration needed (file already v1) ─────
    {
        CrashRecoveryManager mgr3(tmp);
        // The .meta left by mgr2 is already v1, so no migration occurs.
        mgr3.mark_clean_shutdown();
        assert(!mgr3.check_for_crash() &&
               "after clean shutdown, no crash should be detected");
        printf("  Clean shutdown: no crash detected, .meta remains v1.\n");
    }

    // Cleanup
    fs::remove_all(tmp);

    printf("Result: PASS\n");
    return 0;
}
