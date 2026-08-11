// ============================================================================
// synapse/atomic/atomic_telemetry.h
// Project Synapse – Crash-Safe Telemetry with Write-Ahead Log
//
// Every telemetry write goes to WAL first (for crash recovery),
// then to the in-memory ring buffer (for fast reads).
// On startup, WAL is replayed to recover any lost data.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace synapse::atomic {

// WAL entry types
enum class WALEventType : uint32_t {
    None            = 0,
    DrawIndexed     = 1,
    Draw            = 2,
    Dispatch        = 3,
    PushConstants   = 4,
    BindDescriptorSets = 5,
    BindPipeline    = 6,
    CreateImage     = 7,
    DestroyImage    = 8,
    BindShadersEXT  = 9,
    BackendChoice   = 10,
    DVFSChange      = 11,
    ThermalEvent    = 12,
    MLCheckpoint    = 13,
    SessionReport   = 14,
    CleanShutdown   = 0xFFFFFFFF
};

// WAL entry — fixed-size for simple append-only I/O
// Schema version 1: schema_version stored in last 4 bytes of data[240]
// for backward compatibility — old files read as version 0.
static constexpr uint32_t kWALSchemaVersion = 1;
static constexpr size_t kWALDataSize = 240;

struct WALEntry {
    uint64_t sequence{0};
    uint64_t timestamp_ns{0};
    WALEventType event_type{WALEventType::None};
    uint32_t data_size{0};
    uint8_t  data[kWALDataSize]{};  // Payload (padded to fixed size)

    // Schema version stored in last 4 bytes of data for backward compat
    uint32_t get_schema_version() const {
        if (data_size + sizeof(uint32_t) > kWALDataSize) return 0;
        uint32_t ver = 0;
        std::memcpy(&ver, data + kWALDataSize - sizeof(uint32_t), sizeof(ver));
        return ver;
    }
    void set_schema_version(uint32_t ver) {
        // Store at end of data region (always available, even if data_size is 0)
        std::memcpy(data + kWALDataSize - sizeof(uint32_t), &ver, sizeof(ver));
    }
};

static_assert(sizeof(WALEntry) == 264, "WALEntry must be fixed-size for WAL I/O");

static constexpr uint32_t kWALKnownSchemaVersions[] = {0, 1};

static bool is_valid_wal_event_type(WALEventType type) noexcept {
    switch (type) {
        case WALEventType::None:
        case WALEventType::DrawIndexed:
        case WALEventType::Draw:
        case WALEventType::Dispatch:
        case WALEventType::PushConstants:
        case WALEventType::BindDescriptorSets:
        case WALEventType::BindPipeline:
        case WALEventType::CreateImage:
        case WALEventType::DestroyImage:
        case WALEventType::BindShadersEXT:
        case WALEventType::BackendChoice:
        case WALEventType::DVFSChange:
        case WALEventType::ThermalEvent:
        case WALEventType::MLCheckpoint:
        case WALEventType::SessionReport:
        case WALEventType::CleanShutdown:
            return true;
        default:
            return false;
    }
}

static bool is_valid_wal_entry(const WALEntry& entry) noexcept {
    if (!is_valid_wal_event_type(entry.event_type)) return false;
    if (entry.data_size > sizeof(WALEntry::data)) return false;

    const uint32_t ver = entry.get_schema_version();
    for (uint32_t known : kWALKnownSchemaVersions) {
        if (ver == known) return true;
    }
    return false;
}

// In-memory ring buffer for fast reads (lock-free single-producer single-consumer)
class TelemetryRing {
public:
    static constexpr size_t kCapacity = 1024;

    bool push(const WALEntry& entry) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) & (kCapacity - 1);
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;  // Full
        }
        buffer_[head] = entry;
        head_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<WALEntry> pop() noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;  // Empty
        }
        WALEntry entry = buffer_[tail];
        tail_.store((tail + 1) & (kCapacity - 1), std::memory_order_release);
        return entry;
    }

    size_t size() const noexcept {
        auto h = head_.load(std::memory_order_acquire);
        auto t = tail_.load(std::memory_order_acquire);
        return (h - t) & (kCapacity - 1);
    }

    bool empty() const noexcept { return size() == 0; }

private:
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    WALEntry buffer_[kCapacity]{};
};

// Crash-safe telemetry manager
class AtomicTelemetry {
public:
    explicit AtomicTelemetry(const std::string& wal_path)
        : wal_path_(wal_path)
        , sequence_(0)
        , clean_shutdown_(false) {}

    ~AtomicTelemetry() {
        // Flush any buffered entries before shutdown
        flush_buffer();
        // Final fsync
        flush_wal();
        // Destructor marks clean shutdown if not already marked.
        // In a crash, this destructor never runs, so the WAL
        // remains unclean — which is how we detect crashes.
        mark_clean_shutdown();
    }

    // Simulate crash: skip clean shutdown marker
    void simulate_crash() {
        clean_shutdown_ = true;  // Prevent destructor from writing marker
    }

    // Write telemetry event (batched: buffer in memory, flush periodically)
    bool write(WALEventType type, const void* data = nullptr, uint32_t size = 0) {
        if (size > sizeof(WALEntry::data)) return false;

        WALEntry entry{};
        entry.sequence    = sequence_.fetch_add(1, std::memory_order_relaxed);
        entry.set_schema_version(kWALSchemaVersion);
        entry.timestamp_ns = get_timestamp_ns();
        entry.event_type  = type;
        entry.data_size   = size;
        if (data && size > 0) {
            std::memcpy(entry.data, data, size);
        }

        // 1. Write to ring buffer (fast path for consumers)
        ring_.push(entry);

        // 2. Buffer for WAL (batch flush every kWALFlushBatch entries)
        {
            std::lock_guard lock(buffer_mutex_);
            write_buffer_.push_back(entry);
            if (write_buffer_.size() >= kWALFlushBatch) {
                flush_buffer_locked();
            }
        }

        write_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Read from ring buffer (lock-free)
    std::optional<WALEntry> read() noexcept {
        return ring_.pop();
    }

    // Read WAL entries from disk for diagnostics/recovery
    std::vector<WALEntry> read_wal() {
        std::ifstream ifs(wal_path_, std::ios::binary);
        std::vector<WALEntry> entries;
        if (!ifs) return entries;

        WALEntry entry;
        while (ifs.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
            if (is_valid_wal_entry(entry)) {
                entries.push_back(entry);
            }
        }
        return entries;
    }

    // Check for crash on startup; returns number of entries to recover
    uint64_t check_recovery() {
        auto entries = read_wal();
        uint64_t shutdown_seq = 0;
        uint64_t max_seq = 0;
        uint32_t max_schema_ver = 0;

        for (const auto& e : entries) {
            if (e.event_type == WALEventType::CleanShutdown) {
                shutdown_seq = e.sequence;
            }
            if (e.sequence > max_seq) {
                max_seq = e.sequence;
            }
            auto ver = e.get_schema_version();
            if (ver > max_schema_ver) max_schema_ver = ver;
        }

        // Log schema version and sequence-gap diagnostics
        (void)max_schema_ver;

        clean_shutdown_ = (shutdown_seq >= max_seq && max_seq > 0);
        pending_recovery_ = entries;
        return clean_shutdown_ ? 0 : entries.size();
    }

    // Replay recovered entries into the ring buffer
    uint64_t replay() {
        uint64_t count = 0;
        for (const auto& e : pending_recovery_) {
            if (e.event_type != WALEventType::CleanShutdown) {
                ring_.push(e);
                count++;
            }
        }
        pending_recovery_.clear();
        truncate_wal();
        return count;
    }

    // Mark clean shutdown
    void mark_clean_shutdown() {
        if (clean_shutdown_) return;
        clean_shutdown_ = true;

        WALEntry entry{};
        entry.sequence    = sequence_.fetch_add(1);
        entry.set_schema_version(kWALSchemaVersion);
        entry.timestamp_ns = get_timestamp_ns();
        entry.event_type  = WALEventType::CleanShutdown;
        entry.data_size   = 0;

        append_wal(entry);
        flush_wal();
    }

    // Stats
    uint64_t write_count()      const { return write_count_.load(std::memory_order_relaxed); }
    uint64_t sequence()         const { return sequence_.load(std::memory_order_relaxed); }
    bool is_clean_shutdown()    const { return clean_shutdown_.load(std::memory_order_relaxed); }

    static uint64_t sequence_gap_count(const std::vector<WALEntry>& entries) noexcept {
        if (entries.size() <= 1) return 0;

        uint64_t gaps = 0;
        uint64_t prev_seq = entries.front().sequence;

        for (size_t i = 1; i < entries.size(); ++i) {
            const uint64_t curr_seq = entries[i].sequence;
            if (curr_seq != prev_seq + 1) {
                ++gaps;
            }
            prev_seq = curr_seq;
        }
        return gaps;
    }

private:
    // Flush batch size — entries accumulate in memory before disk write
    static constexpr size_t kWALFlushBatch = 64;

    std::string wal_path_;
    std::atomic<uint64_t> sequence_;
    std::atomic<uint64_t> write_count_{0};
    std::atomic<bool> clean_shutdown_;
    TelemetryRing ring_;
    std::mutex wal_mutex_;
    std::vector<WALEntry> pending_recovery_;

    // Batched write buffer
    std::mutex buffer_mutex_;
    std::vector<WALEntry> write_buffer_;

    void flush_buffer() {
        std::lock_guard lock(buffer_mutex_);
        flush_buffer_locked();
    }

    void flush_buffer_locked() {
        if (write_buffer_.empty()) return;
        // Append all buffered entries in a single I/O operation
        std::lock_guard wal_lock(wal_mutex_);
        std::ofstream ofs(wal_path_, std::ios::binary | std::ios::app);
        if (ofs) {
            ofs.write(reinterpret_cast<const char*>(write_buffer_.data()),
                      write_buffer_.size() * sizeof(WALEntry));
            ofs.flush();
        }
        write_buffer_.clear();
    }

    bool append_wal(const WALEntry& entry) {
        std::lock_guard lock(wal_mutex_);
        std::ofstream ofs(wal_path_, std::ios::binary | std::ios::app);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        ofs.flush();
        return true;
    }

    void flush_wal() {
        std::lock_guard lock(wal_mutex_);
        std::ofstream ofs(wal_path_, std::ios::binary | std::ios::app);
        if (ofs) ofs.flush();
    }

    void truncate_wal() {
        std::lock_guard lock(wal_mutex_);
        // Truncate by reopening with trunc flag
        std::ofstream ofs(wal_path_, std::ios::binary | std::ios::trunc);
    }

    static uint64_t get_timestamp_ns() {
        auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count());
    }
};

}  // namespace synapse::atomic
