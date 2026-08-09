// ============================================================================
// synapse/recovery/watchdog.h
// Project Synapse – Process Watchdog with Heartbeat Monitoring
//
// Monitors the shim process via shared-memory heartbeat counter.
// If heartbeat stops, triggers recovery: signal → wait → restart.
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace synapse::recovery {

// Shared memory for IPC between watchdog and shim (same process for now)
struct HeartbeatState {
    alignas(64) std::atomic<uint64_t> counter{0};
    alignas(64) std::atomic<uint64_t> last_time_ns{0};
    alignas(64) std::atomic<bool> shim_alive{false};
    alignas(64) std::atomic<bool> watchdog_alive{false};
};

class Watchdog {
public:
    explicit Watchdog(HeartbeatState* state, uint32_t timeout_ms = 5000)
        : state_(state)
        , timeout_ms_(timeout_ms)
        , running_(false) {}

    ~Watchdog() { stop(); }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    void start() {
        if (running_.load()) return;
        running_ = true;
        state_->watchdog_alive.store(true, std::memory_order_release);
        thread_ = std::thread([this]() { monitor_loop(); });
    }

    void stop() {
        running_ = false;
        state_->watchdog_alive.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    // Called by the shim to signal liveness
    void heartbeat() {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        auto ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        state_->counter.fetch_add(1, std::memory_order_relaxed);
        state_->last_time_ns.store(ns, std::memory_order_release);
        state_->shim_alive.store(true, std::memory_order_release);
    }

    // Register a callback to execute on recovery
    using RecoveryCallback = std::function<void()>;
    void on_recovery(RecoveryCallback cb) {
        callbacks_.push_back(std::move(cb));
    }

    // Status query
    struct Status {
        bool shim_responding;
        uint64_t heartbeat_count;
        uint64_t last_heartbeat_age_ms;
        bool watchdog_active;
    };

    Status status() const {
        Status s{};
        s.shim_responding = state_->shim_alive.load(std::memory_order_acquire);
        s.heartbeat_count = state_->counter.load(std::memory_order_acquire);

        auto now_ns = std::chrono::steady_clock::now().time_since_epoch();
        auto now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now_ns).count());
        auto last = state_->last_time_ns.load(std::memory_order_acquire);
        s.last_heartbeat_age_ms = (now - last) / 1000000;
        s.watchdog_active = state_->watchdog_alive.load(std::memory_order_acquire);
        return s;
    }

private:
    HeartbeatState* state_;
    uint32_t timeout_ms_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::vector<RecoveryCallback> callbacks_;

    void monitor_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            auto s = status();
            if (s.shim_responding && s.last_heartbeat_age_ms < timeout_ms_) {
                // Healthy
            } else if (s.shim_responding) {
                // Heartbeat timeout — shim may be stuck
                trigger_recovery();
            } else if (!s.shim_responding && s.heartbeat_count > 0) {
                // Shim exited — trigger recovery
                trigger_recovery();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    void trigger_recovery() {
        for (auto& cb : callbacks_) {
            cb();
        }
    }
};

}  // namespace synapse::recovery
