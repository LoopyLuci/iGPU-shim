/**
 * @file test_schema_migration_multihop.cpp
 * @brief Multi-hop schema migration test: v0→v1→v2→v3 with realistic payloads.
 *
 * Validates that CrashRecoveryManager-style migration chains work end-to-end:
 *   v0: raw RecoveryMetadata (32 bytes, no header)
 *   v1: [MessageHeader][RecoveryMetadata] (header + 32 bytes)
 *   v2: [MessageHeader][RecoveryMetadata + crash_reason + recovery_method] (header + 48 bytes)
 *   v3: [MessageHeader][RecoveryMetadata + crash_reason + recovery_method + hardware_hash] (header + 56 bytes)
 */

#include "../protocol/schema_migration.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

using namespace synapse::protocol;

// ── Simulated RecoveryMetadata (v0 layout, 32 bytes) ──────────────────
struct RecoveryMetadataV0 {
    uint64_t last_clean_shutdown_sequence{0};
    uint64_t last_wal_sequence{0};
    uint64_t total_recoveries{0};
    uint64_t total_entries_recovered{0};
};

// ── v2 extension: crash_reason + recovery_method (16 bytes) ───────────
struct V2Fields {
    uint32_t crash_reason{0};       // 0=none, 1=timeout, 2=exception, 3=device_lost
    uint32_t recovery_method{0};    // 0=none, 1=wal_replay, 2=snapshot, 3=full_reset
};

// ── v3 extension: hardware_hash (4 bytes) ─────────────────────────────
struct V3Fields {
    uint32_t hardware_hash{0};      // FNV-1a of GPU device ID string
};

// ── Helpers ───────────────────────────────────────────────────────────

static std::vector<uint8_t> struct_to_bytes(const void* p, size_t len) {
    const auto* raw = reinterpret_cast<const uint8_t*>(p);
    return std::vector<uint8_t>(raw, raw + len);
}

template<typename T>
static T bytes_to_struct(const std::vector<uint8_t>& data, size_t offset = 0) {
    T val{};
    if (data.size() >= offset + sizeof(T)) {
        std::memcpy(&val, data.data() + offset, sizeof(T));
    }
    return val;
}

static uint32_t fnv1a(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (auto c : s) { hash ^= static_cast<uint8_t>(c); hash *= 16777619u; }
    return hash;
}

static MessageHeader make_header(uint32_t version, uint32_t payload_size) {
    MessageHeader hdr{};
    hdr.magic = 0x53594E41;  // "SYNA"
    hdr.schema_version = version;
    hdr.payload_size = payload_size;
    // Checksum over payload (caller fills after)
    return hdr;
}

// ── Build v0 payload: raw RecoveryMetadataV0 (32 bytes, no header) ────

static std::vector<uint8_t> build_v0_payload(uint64_t clean_seq, uint64_t wal_seq,
                                               uint64_t recoveries, uint64_t entries) {
    RecoveryMetadataV0 m{};
    m.last_clean_shutdown_sequence = clean_seq;
    m.last_wal_sequence = wal_seq;
    m.total_recoveries = recoveries;
    m.total_entries_recovered = entries;
    return struct_to_bytes(&m, sizeof(m));
}

// ── Build v1 payload: [MessageHeader][RecoveryMetadataV0] ─────────────

static std::vector<uint8_t> build_v1_file(uint64_t clean_seq, uint64_t wal_seq,
                                            uint64_t recoveries, uint64_t entries) {
    auto payload = build_v0_payload(clean_seq, wal_seq, recoveries, entries);
    auto hdr = make_header(1, static_cast<uint32_t>(payload.size()));
    // Compute checksum
    uint32_t hash = 2166136261u;
    for (auto b : payload) { hash ^= b; hash *= 16777619u; }
    hdr.checksum = hash;

    std::vector<uint8_t> file(sizeof(hdr) + payload.size());
    std::memcpy(file.data(), &hdr, sizeof(hdr));
    std::memcpy(file.data() + sizeof(hdr), payload.data(), payload.size());
    return file;
}

// ── Build v2 payload: [MessageHeader][RecoveryMetadataV0 + V2Fields] ──

static std::vector<uint8_t> build_v2_file(uint64_t clean_seq, uint64_t wal_seq,
                                            uint64_t recoveries, uint64_t entries,
                                            uint32_t crash_reason, uint32_t recovery_method) {
    auto base = build_v0_payload(clean_seq, wal_seq, recoveries, entries);
    V2Fields v2{};
    v2.crash_reason = crash_reason;
    v2.recovery_method = recovery_method;
    auto v2_bytes = struct_to_bytes(&v2, sizeof(v2));

    std::vector<uint8_t> payload(base.size() + v2_bytes.size());
    std::memcpy(payload.data(), base.data(), base.size());
    std::memcpy(payload.data() + base.size(), v2_bytes.data(), v2_bytes.size());

    auto hdr = make_header(2, static_cast<uint32_t>(payload.size()));
    uint32_t hash = 2166136261u;
    for (auto b : payload) { hash ^= b; hash *= 16777619u; }
    hdr.checksum = hash;

    std::vector<uint8_t> file(sizeof(hdr) + payload.size());
    std::memcpy(file.data(), &hdr, sizeof(hdr));
    std::memcpy(file.data() + sizeof(hdr), payload.data(), payload.size());
    return file;
}

// ── Build v3 payload: [MessageHeader][RecoveryMetadataV0 + V2Fields + V3Fields] ──

static std::vector<uint8_t> build_v3_file(uint64_t clean_seq, uint64_t wal_seq,
                                            uint64_t recoveries, uint64_t entries,
                                            uint32_t crash_reason, uint32_t recovery_method,
                                            uint32_t hardware_hash) {
    auto base = build_v0_payload(clean_seq, wal_seq, recoveries, entries);
    V2Fields v2{};
    v2.crash_reason = crash_reason;
    v2.recovery_method = recovery_method;
    auto v2_bytes = struct_to_bytes(&v2, sizeof(v2));

    V3Fields v3{};
    v3.hardware_hash = hardware_hash;
    auto v3_bytes = struct_to_bytes(&v3, sizeof(v3));

    std::vector<uint8_t> payload(base.size() + v2_bytes.size() + v3_bytes.size());
    std::memcpy(payload.data(), base.data(), base.size());
    std::memcpy(payload.data() + base.size(), v2_bytes.data(), v2_bytes.size());
    std::memcpy(payload.data() + base.size() + v2_bytes.size(), v3_bytes.data(), v3_bytes.size());

    auto hdr = make_header(3, static_cast<uint32_t>(payload.size()));
    uint32_t hash = 2166136261u;
    for (auto b : payload) { hash ^= b; hash *= 16777619u; }
    hdr.checksum = hash;

    std::vector<uint8_t> file(sizeof(hdr) + payload.size());
    std::memcpy(file.data(), &hdr, sizeof(hdr));
    std::memcpy(file.data() + sizeof(hdr), payload.data(), payload.size());
    return file;
}

// ── Test runner ───────────────────────────────────────────────────────

int main() {
    SchemaMigration schema;

    // Register multi-hop chain: v0→v1→v2→v3
    // v0→v1: wrap raw struct with MessageHeader
    schema.register_migration(0, 1, [](const std::vector<uint8_t>& data) -> std::vector<uint8_t> {
        MessageHeader hdr{};
        hdr.magic = 0x53594E41;
        hdr.schema_version = 1;
        hdr.payload_size = static_cast<uint32_t>(data.size());
        uint32_t hash = 2166136261u;
        for (auto b : data) { hash ^= b; hash *= 16777619u; }
        hdr.checksum = hash;

        std::vector<uint8_t> result(sizeof(hdr) + data.size());
        std::memcpy(result.data(), &hdr, sizeof(hdr));
        std::memcpy(result.data() + sizeof(hdr), data.data(), data.size());
        return result;
    });

    // v1→v2: append V2Fields (crash_reason + recovery_method)
    schema.register_migration(1, 2, [](const std::vector<uint8_t>& data) -> std::vector<uint8_t> {
        // data is raw payload (without header) from v1
        // Append V2Fields with default values (migration from v1 → v2 adds new fields)
        V2Fields v2{};
        auto v2_bytes = struct_to_bytes(&v2, sizeof(v2));

        std::vector<uint8_t> result(data.size() + v2_bytes.size());
        std::memcpy(result.data(), data.data(), data.size());
        std::memcpy(result.data() + data.size(), v2_bytes.data(), v2_bytes.size());
        return result;
    });

    // v2→v3: append V3Fields (hardware_hash)
    schema.register_migration(2, 3, [](const std::vector<uint8_t>& data) -> std::vector<uint8_t> {
        V3Fields v3{};
        v3.hardware_hash = fnv1a("Intel UHD Graphics 630");  // default hash for migration
        auto v3_bytes = struct_to_bytes(&v3, sizeof(v3));

        std::vector<uint8_t> result(data.size() + v3_bytes.size());
        std::memcpy(result.data(), data.data(), data.size());
        std::memcpy(result.data() + data.size(), v3_bytes.data(), v3_bytes.size());
        return result;
    });

    // ── Test 1: v0 → v3 multi-hop migration ───────────────────────────
    std::cout << "Test 1: v0 → v3 multi-hop migration... ";
    {
        auto v0 = build_v0_payload(100, 50, 3, 45);
        auto v3 = schema.migrate(v0, 0, 3);

        // v3 payload should be: RecoveryMetadataV0 (32) + V2Fields (16) + V3Fields (4) = 52 bytes
        assert(v3.size() == 52);

        // Verify base fields preserved
        auto meta = bytes_to_struct<RecoveryMetadataV0>(v3, 0);
        assert(meta.last_clean_shutdown_sequence == 100);
        assert(meta.last_wal_sequence == 50);
        assert(meta.total_recoveries == 3);
        assert(meta.total_entries_recovered == 45);

        // Verify v2 fields (default values from migration)
        auto v2f = bytes_to_struct<V2Fields>(v3, 32);
        assert(v2f.crash_reason == 0);
        assert(v2f.recovery_method == 0);

        // Verify v3 fields (hardware_hash computed during migration)
        auto v3f = bytes_to_struct<V3Fields>(v3, 48);
        assert(v3f.hardware_hash == fnv1a("Intel UHD Graphics 630"));

        std::cout << "PASS\n";
    }

    // ── Test 2: v1 → v3 migration ─────────────────────────────────────
    std::cout << "Test 2: v1 → v3 migration... ";
    {
        // v1 file: header + 32-byte payload
        auto v1_file = build_v1_file(200, 100, 7, 90);

        // Extract payload (skip header)
        std::vector<uint8_t> payload(v1_file.begin() + sizeof(MessageHeader), v1_file.end());
        auto v3 = schema.migrate(payload, 1, 3);

        assert(v3.size() == 52);

        auto meta = bytes_to_struct<RecoveryMetadataV0>(v3, 0);
        assert(meta.last_clean_shutdown_sequence == 200);
        assert(meta.last_wal_sequence == 100);
        assert(meta.total_recoveries == 7);
        assert(meta.total_entries_recovered == 90);

        auto v3f = bytes_to_struct<V3Fields>(v3, 48);
        assert(v3f.hardware_hash == fnv1a("Intel UHD Graphics 630"));

        std::cout << "PASS\n";
    }

    // ── Test 3: v2 → v3 migration (single hop) ────────────────────────
    std::cout << "Test 3: v2 → v3 migration (single hop)... ";
    {
        auto v2_file = build_v2_file(300, 150, 12, 180, 2, 1);

        std::vector<uint8_t> payload(v2_file.begin() + sizeof(MessageHeader), v2_file.end());
        auto v3 = schema.migrate(payload, 2, 3);

        assert(v3.size() == 52);

        auto meta = bytes_to_struct<RecoveryMetadataV0>(v3, 0);
        assert(meta.last_clean_shutdown_sequence == 300);
        assert(meta.total_recoveries == 12);

        // v2 fields should be preserved (not defaults)
        auto v2f = bytes_to_struct<V2Fields>(v3, 32);
        assert(v2f.crash_reason == 2);
        assert(v2f.recovery_method == 1);

        // v3 fields added by migration
        auto v3f = bytes_to_struct<V3Fields>(v3, 48);
        assert(v3f.hardware_hash == fnv1a("Intel UHD Graphics 630"));

        std::cout << "PASS\n";
    }

    // ── Test 4: v3 → v3 identity (no migration needed) ────────────────
    std::cout << "Test 4: v3 → v3 identity (no migration needed)... ";
    {
        auto v3_file = build_v3_file(400, 200, 20, 300, 3, 2, 0xDEADBEEF);

        std::vector<uint8_t> payload(v3_file.begin() + sizeof(MessageHeader), v3_file.end());
        auto result = schema.migrate(payload, 3, 3);

        // Should return the same data
        assert(result == payload);

        std::cout << "PASS\n";
    }

    // ── Test 5: Path existence for all hop combinations ────────────────
    std::cout << "Test 5: Path existence for all hop combinations... ";
    {
        assert(schema.has_path(0, 1));
        assert(schema.has_path(0, 2));
        assert(schema.has_path(0, 3));
        assert(schema.has_path(1, 2));
        assert(schema.has_path(1, 3));
        assert(schema.has_path(2, 3));
        assert(schema.has_path(3, 3));

        // No reverse paths
        assert(!schema.has_path(1, 0));
        assert(!schema.has_path(2, 0));
        assert(!schema.has_path(3, 0));
        assert(!schema.has_path(2, 1));
        assert(!schema.has_path(3, 1));
        assert(!schema.has_path(3, 2));

        std::cout << "PASS\n";
    }

    // ── Test 6: Backward compatibility ─────────────────────────────────
    std::cout << "Test 6: Backward compatibility (v3 reader reads v2 data)... ";
    {
        // v3 reader can read v2 data (within 1 version)
        assert(schema.is_compatible(3, 3) == true);
        assert(schema.is_compatible(3, 2) == true);
        assert(schema.is_compatible(3, 1) == false);  // 2 versions behind = incompatible
        assert(schema.is_compatible(3, 0) == false);

        // v2 reader can read v1 data
        assert(schema.is_compatible(2, 2) == true);
        assert(schema.is_compatible(2, 1) == true);
        assert(schema.is_compatible(2, 0) == false);

        std::cout << "PASS\n";
    }

    // ── Test 7: Registry metadata ──────────────────────────────────────
    std::cout << "Test 7: Registry metadata... ";
    {
        assert(schema.latest_version() == 3);
        assert(schema.migration_count() == 3);  // v0→v1, v1→v2, v2→v3
        std::cout << "PASS\n";
    }

    // ── Test 8: Downgrade throws ───────────────────────────────────────
    std::cout << "Test 8: Downgrade from v3 to v0 throws... ";
    {
        bool threw = false;
        try {
            auto v3_file = build_v3_file(1, 1, 1, 1, 0, 0, 0);
            std::vector<uint8_t> payload(v3_file.begin() + sizeof(MessageHeader), v3_file.end());
            schema.migrate(payload, 3, 0);
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
        std::cout << "PASS\n";
    }

    // ── Test 9: Missing intermediate step throws ───────────────────────
    std::cout << "Test 9: Missing intermediate step (v0→v3 with only v0→v1 registered)... ";
    {
        SchemaMigration partial;
        partial.register_migration(0, 1, [](const std::vector<uint8_t>& data) {
            return data;  // identity
        });
        // No v1→v2 or v2→v3 registered, so v0→v3 should fail

        bool threw = false;
        try {
            partial.migrate(std::vector<uint8_t>{1, 2, 3}, 0, 3);
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
        std::cout << "PASS\n";
    }

    // ── Test 10: Large payload multi-hop ───────────────────────────────
    std::cout << "Test 10: Large payload multi-hop (1 KB v0 → v3)... ";
    {
        // Build a 1 KB v0 payload
        std::vector<uint8_t> large_v0(1024);
        for (size_t i = 0; i < large_v0.size(); ++i) {
            large_v0[i] = static_cast<uint8_t>(i & 0xFF);
        }

        auto v3 = schema.migrate(large_v0, 0, 3);
        // v3 = 1024 (base) + 16 (V2Fields) + 4 (V3Fields) = 1044 bytes
        assert(v3.size() == 1044);

        // Verify first 32 bytes match original RecoveryMetadataV0 layout
        RecoveryMetadataV0 meta{};
        std::memcpy(&meta, v3.data(), sizeof(meta));
        // The large payload starts with garbage, but the struct fields are at fixed offsets
        // Just verify the payload size is correct and data is preserved
        for (size_t i = 0; i < 1024; ++i) {
            assert(v3[i] == static_cast<uint8_t>(i & 0xFF));
        }

        std::cout << "PASS\n";
    }

    std::cout << "\nAll 10 tests passed.\n";
    std::cout << "Result: PASS\n";
    return 0;
}
