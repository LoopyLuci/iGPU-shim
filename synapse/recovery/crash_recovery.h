// ============================================================================
// synapse/recovery/crash_recovery.h
// Project Synapse – Crash Recovery Manager with WAL Replay
//
// On startup, checks for unclean shutdown via WAL metadata.
// If crash detected, replays WAL entries to restore state.
// Provides recovery status and metadata persistence.
// RecoveryMetadata is schema-versioned via SchemaMigration.
// ============================================================================
#pragma once

#include "../atomic/atomic_telemetry.h"
#include "../protocol/schema_migration.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace synapse::recovery {

// Schema version for persisted data formats
static constexpr uint32_t kRecoverySchemaVersion = 1;

// Recovery metadata — persisted alongside WAL
struct RecoveryMetadata {
    uint64_t last_clean_shutdown_sequence{0};
    uint64_t last_wal_sequence{0};
    uint64_t total_recoveries{0};
    uint64_t total_entries_recovered{0};
};

// Versioned file format: [MessageHeader][RecoveryMetadata]
// On first load, if no header is found, treat as v0 (legacy) and migrate to v1.

class CrashRecoveryManager {
public:
    explicit CrashRecoveryManager(const std::string& data_directory)
        : data_dir_(data_directory)
        , metadata_path_(data_directory + "/synapse_recovery.meta")
        , telemetry_(data_directory + "/synapse.wal")
    {
        // Register migration: v0 (legacy raw struct) → v1 (header + struct)
        schema_.register_migration(0, 1, [](const std::vector<uint8_t>& data) -> std::vector<uint8_t> {
            // Legacy format: raw RecoveryMetadata without header
            // Migrate by wrapping with MessageHeader
            protocol::MessageHeader hdr{};
            hdr.magic = 0x53594E41;  // "SYNA"
            hdr.schema_version = 1;
            hdr.payload_size = static_cast<uint32_t>(data.size());
            // Compute checksum
            uint32_t hash = 2166136261u;
            for (auto b : data) { hash ^= b; hash *= 16777619u; }
            hdr.checksum = hash;

            std::vector<uint8_t> result(sizeof(hdr) + data.size());
            std::memcpy(result.data(), &hdr, sizeof(hdr));
            std::memcpy(result.data() + sizeof(hdr), data.data(), data.size());
            return result;
        });
    }

    // Check for crash on startup; returns true if recovery needed
    bool check_for_crash() {
        metadata_ = load_metadata();
        uint64_t pending = telemetry_.check_recovery();
        return pending > 0;
    }

    // Perform recovery: replay WAL entries
    struct RecoveryResult {
        bool success{false};
        uint64_t entries_recovered{0};
        uint64_t last_sequence{0};
        std::string error;
    };

    RecoveryResult recover() {
        RecoveryResult result;
        uint64_t pending = telemetry_.check_recovery();

        if (pending == 0) {
            result.success = true;
            return result;  // No recovery needed
        }

        // Replay WAL into live system
        result.entries_recovered = telemetry_.replay();
        result.last_sequence = telemetry_.sequence();
        result.success = true;

        // Update metadata
        metadata_.total_recoveries++;
        metadata_.total_entries_recovered += result.entries_recovered;
        metadata_.last_wal_sequence = result.last_sequence;
        save_metadata();

        return result;
    }

    // Mark clean shutdown (call in destructor or shutdown handler)
    void mark_clean_shutdown() {
        telemetry_.mark_clean_shutdown();
        metadata_.last_clean_shutdown_sequence = telemetry_.sequence();
        save_metadata();
    }

    // Access the underlying telemetry for writes
    atomic::AtomicTelemetry& telemetry() { return telemetry_; }

    // Get recovery metadata
    const RecoveryMetadata& metadata() const { return metadata_; }

    // Schema migration accessor
    protocol::SchemaMigration& schema() { return schema_; }
    uint32_t schema_version() const { return kRecoverySchemaVersion; }

private:
    std::string data_dir_;
    std::string metadata_path_;
    RecoveryMetadata metadata_;
    atomic::AtomicTelemetry telemetry_;
    protocol::SchemaMigration schema_;

    RecoveryMetadata load_metadata() {
        RecoveryMetadata m;
        std::ifstream ifs(metadata_path_, std::ios::binary);
        if (!ifs) return m;

        // Read raw bytes
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(ifs)),
                                  std::istreambuf_iterator<char>());

        if (raw.empty()) return m;

        // Check if this is versioned (starts with "SYNA" magic)
        if (raw.size() >= sizeof(protocol::MessageHeader)) {
            protocol::MessageHeader hdr{};
            std::memcpy(&hdr, raw.data(), sizeof(hdr));

            if (hdr.magic == 0x53594E41 && hdr.schema_version > 0) {
                // Versioned format — check compatibility
                if (!schema_.is_compatible(kRecoverySchemaVersion, hdr.schema_version)) {
                    // Incompatible — try migration
                    if (schema_.has_path(hdr.schema_version, kRecoverySchemaVersion)) {
                        std::vector<uint8_t> payload(
                            raw.begin() + sizeof(protocol::MessageHeader), raw.end());
                        auto migrated = schema_.migrate(payload, hdr.schema_version, kRecoverySchemaVersion);
                        if (migrated.size() >= sizeof(RecoveryMetadata)) {
                            std::memcpy(&m, migrated.data(), sizeof(RecoveryMetadata));
                        }
                    }
                    // If no migration path, return defaults (safe fallback)
                } else {
                    // Compatible — read payload directly
                    std::vector<uint8_t> payload(
                        raw.begin() + sizeof(protocol::MessageHeader), raw.end());
                    if (payload.size() >= sizeof(RecoveryMetadata)) {
                        std::memcpy(&m, payload.data(), sizeof(RecoveryMetadata));
                    }
                }
                return m;
            }
        }

        // Legacy format (v0): raw RecoveryMetadata without header
        // Migrate in-memory for this session; will be saved as v1 next time
        if (raw.size() >= sizeof(RecoveryMetadata)) {
            RecoveryMetadata legacy{};
            std::memcpy(&legacy, raw.data(), sizeof(RecoveryMetadata));

            // Migrate: wrap with header and save as v1
            std::vector<uint8_t> legacy_bytes(raw.begin(),
                raw.begin() + sizeof(RecoveryMetadata));
            auto migrated = schema_.migrate(legacy_bytes, 0, kRecoverySchemaVersion);

            // Save migrated format
            {
                std::ofstream ofs(metadata_path_, std::ios::binary | std::ios::trunc);
                if (ofs) {
                    ofs.write(reinterpret_cast<const char*>(migrated.data()), migrated.size());
                }
            }

            return legacy;
        }

        return m;
    }

    void save_metadata() {
        // Ensure directory exists
        std::error_code ec;
        std::filesystem::create_directories(data_dir_, ec);

        // Serialize with MessageHeader (v1 format)
        const auto* raw = reinterpret_cast<const uint8_t*>(&metadata_);
        std::vector<uint8_t> payload(raw, raw + sizeof(RecoveryMetadata));

        protocol::MessageHeader hdr{};
        hdr.magic = 0x53594E41;
        hdr.schema_version = kRecoverySchemaVersion;
        hdr.payload_size = static_cast<uint32_t>(payload.size());
        // Compute checksum
        uint32_t hash = 2166136261u;
        for (auto b : payload) { hash ^= b; hash *= 16777619u; }
        hdr.checksum = hash;

        std::ofstream ofs(metadata_path_, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
            ofs.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        }
    }
};

}  // namespace synapse::recovery
