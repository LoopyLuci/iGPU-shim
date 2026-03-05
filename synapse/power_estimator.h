// ============================================================================
// synapse/power_estimator.h
// Project Synapse – Capstone Metric: Energy Impact
// ============================================================================
#pragma once
#include "platform_config.h"   // pj_per_bit sourced from per-SKU config (Risk #3 fix)

namespace synapse::metrics {

class PowerEstimator {
public:
    /// @brief Energy constant sourced from PlatformConfig — not a compile-time literal.
    double pj_per_bit() const { return PlatformConfig::get().pj_per_bit; }

    void log_transaction(uint64_t full_bytes, uint64_t actual_bytes) {
        const double ppb = pj_per_bit();
        total_potential_energy_pj_ += (full_bytes  * 8 * ppb);
        total_actual_energy_pj_    += (actual_bytes * 8 * ppb);
    }

    void log_switch_overhead(double overhead_nj) {
        total_actual_energy_pj_ += overhead_nj * 1000.0; // nJ -> pJ
    }

    struct PowerReport {
        double joules_saved;
        double avg_milliwatts_saved_at_60fps;
        double battery_extension_factor; // Theoretical
    };

    PowerReport generate() const {
        double saved_pj = total_potential_energy_pj_ - total_actual_energy_pj_;
        double saved_j  = saved_pj / 1e12; // Pico -> Joules

        // 60 FPS = 1/60 s per frame.
        // Power (W) = Energy (J) / Time (s)
        const double frame_time_s = 1.0 / 60.0;
        double mw_saved = (total_frames_ > 0)
            ? (saved_j / (total_frames_ * frame_time_s)) * 1000.0
            : 0.0;

        const double extension = (total_potential_energy_pj_ > 0)
            ? saved_pj / total_potential_energy_pj_
            : 0.0;

        return { saved_j, mw_saved, extension };
    }

    void increment_frame() { total_frames_++; }

private:
    uint64_t total_frames_              = 0;
    double   total_potential_energy_pj_ = 0;
    double   total_actual_energy_pj_    = 0;
};

} // namespace synapse::metrics