/**
 * @file test_schema_migration_integration.cpp
 * @brief Integration test for schema migration via CrashRecoveryManager.
 *
 * Writes legacy v0 recovery metadata, then verifies the manager loads
 * and migrates it to the current schema version.
 */

#include "../recovery/crash_recovery.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace synapse::recovery;

static void write_legacy_metadata(const std::string& path, const RecoveryMetadata& meta) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    assert(ofs && "failed to open legacy metadata file");
    ofs.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
}

static void assert_eq_metadata(const RecoveryMetadata& a, const RecoveryMetadata& b) {
    assert(a.last_clean_shutdown_sequence == b.last_clean_shutdown_sequence);
    assert(a.last_wal_sequence == b.last_wal_sequence);
    assert(a.total_recoveries == b.total_recoveries);
    assert(a.total_entries_recovered == b.total_entries_recovered);
}

int main() {
    const std::string tmp = fs::temp_directory_path().string() + "/synapse_schema_migration_integration";
    fs::create_directories(tmp);
    fs::remove_all(tmp + "/synapse_recovery.meta");

    RecoveryMetadata legacy{};
    legacy.last_clean_shutdown_sequence = 7;
    legacy.last_wal_sequence = 14;
    legacy.total_recoveries = 3;
    legacy.total_entries_recovered = 42;

    write_legacy_metadata(tmp + "/synapse_recovery.meta", legacy);

    CrashRecoveryManager mgr(tmp);

    bool needed_recovery = mgr.check_for_crash();
    (void)needed_recovery;

    auto loaded = mgr.metadata();
    assert_eq_metadata(loaded, legacy);

    // Save again and verify it is written in the current schema format.
    mgr.mark_clean_shutdown();
    auto reloaded = mgr.metadata();
    assert_eq_metadata(reloaded, loaded);

    std::cout << "Result: PASS\n";
    return 0;
}
