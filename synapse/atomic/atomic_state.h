// ============================================================================
// synapse/atomic/atomic_state.h
// Project Synapse – Atomic State Machine with CAS Transitions
//
// All state transitions are atomic, thread-safe, and auditable.
// Every transition is logged with timestamp, thread ID, and reason.
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace synapse::atomic {

// State transition record for audit trail
struct Transition {
    uint32_t from_state;
    uint32_t to_state;
    uint64_t timestamp_ns;
    uint32_t thread_id;
    const char* reason;
    uint32_t error_code;
};

// Valid shim states
enum class ShimState : uint32_t {
    Uninitialized = 0,
    Initializing  = 1,
    Active        = 2,
    Degraded      = 3,   // Recoverable error
    Suspended     = 4,   // Manual intervention needed
    ShuttingDown  = 5,
    Crashed       = 6
};

// Atomic state machine with CAS transitions and full audit trail
class AtomicStateMachine {
public:
    explicit AtomicStateMachine(ShimState initial = ShimState::Uninitialized)
        : current_state_(static_cast<uint32_t>(initial))
        , transition_count_(0) {}

    // Thread-safe state transition with CAS
    bool transition(ShimState target, const char* reason, uint32_t error = 0) {
        uint32_t expected = current_state_.load(std::memory_order_relaxed);

        if (!is_valid_transition(static_cast<ShimState>(expected), target)) {
            return false;
        }

        if (!current_state_.compare_exchange_strong(
                expected,
                static_cast<uint32_t>(target),
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return false;  // Another thread transitioned first
        }

        log_transition(expected, static_cast<uint32_t>(target), reason, error);
        transition_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Read current state
    ShimState current() const {
        return static_cast<ShimState>(current_state_.load(std::memory_order_acquire));
    }

    // Get transition history (thread-safe copy)
    std::vector<Transition> history() const {
        std::lock_guard lock(history_mutex_);
        return history_;
    }

    uint64_t transition_count() const {
        return transition_count_.load(std::memory_order_relaxed);
    }

    // Check if a transition is valid
    static bool is_valid_transition(ShimState from, ShimState to) {
        switch (from) {
            case ShimState::Uninitialized:
                return to == ShimState::Initializing;
            case ShimState::Initializing:
                return to == ShimState::Active || to == ShimState::Crashed;
            case ShimState::Active:
                return to == ShimState::Degraded || to == ShimState::Suspended ||
                       to == ShimState::ShuttingDown || to == ShimState::Crashed;
            case ShimState::Degraded:
                return to == ShimState::Active || to == ShimState::Suspended ||
                       to == ShimState::Crashed;
            case ShimState::Suspended:
                return to == ShimState::Active || to == ShimState::Crashed;
            case ShimState::ShuttingDown:
                return to == ShimState::Uninitialized;
            case ShimState::Crashed:
                return to == ShimState::Uninitialized;  // Recovery only
            default:
                return false;
        }
    }

    // Human-readable state name
    static const char* state_name(ShimState s) {
        switch (s) {
            case ShimState::Uninitialized: return "Uninitialized";
            case ShimState::Initializing:  return "Initializing";
            case ShimState::Active:        return "Active";
            case ShimState::Degraded:      return "Degraded";
            case ShimState::Suspended:     return "Suspended";
            case ShimState::ShuttingDown:  return "ShuttingDown";
            case ShimState::Crashed:       return "Crashed";
            default:                       return "Unknown";
        }
    }

private:
    std::atomic<uint32_t> current_state_;
    std::atomic<uint64_t> transition_count_;

    mutable std::mutex history_mutex_;
    std::vector<Transition> history_;

    void log_transition(uint32_t from, uint32_t to, const char* reason, uint32_t error) {
        Transition t{};
        t.from_state  = from;
        t.to_state    = to;
        t.timestamp_ns = get_timestamp_ns();
        t.thread_id   = get_thread_id();
        t.reason      = reason;
        t.error_code  = error;

        std::lock_guard lock(history_mutex_);
        history_.push_back(t);
    }

    static uint64_t get_timestamp_ns() {
        auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count());
    }

    static uint32_t get_thread_id() {
#ifdef _WIN32
        return static_cast<uint32_t>(GetCurrentThreadId());
#else
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#endif
    }
};

}  // namespace synapse::atomic
