#pragma once

#include <cstdint>
#include <atomic>
#include <array>

namespace synapse::testing {

class CoverageTracker {
public:
    void note_backend_choice(int backend_id) { backend_counts_[backend_id].fetch_add(1, std::memory_order_relaxed); }
    void note_dvfs_transition() { dvfs_transitions_.fetch_add(1, std::memory_order_relaxed); }
    void note_thermal_event() { thermal_events_.fetch_add(1, std::memory_order_relaxed); }
    void note_cache_miss() { cache_misses_.fetch_add(1, std::memory_order_relaxed); }

    float path_coverage_percent() const {
        // Simple heuristic: if all counters > 0 then full coverage
        int covered = 0;
        for (auto &c : backend_counts_) if (c.load(std::memory_order_relaxed) > 0) ++covered;
        if (dvfs_transitions_.load() > 0) ++covered;
        if (thermal_events_.load() > 0) ++covered;
        if (cache_misses_.load() > 0) ++covered;
        const int total = static_cast<int>(backend_counts_.size()) + 3;
        return (static_cast<float>(covered) / total) * 100.0f;
    }

private:
    std::array<std::atomic<uint32_t>, 3> backend_counts_{{0,0,0}}; // JIT, HAI, Oracle
    std::atomic<uint32_t> dvfs_transitions_{0};
    std::atomic<uint32_t> thermal_events_{0};
    std::atomic<uint32_t> cache_misses_{0};
};

} // namespace synapse::testing
