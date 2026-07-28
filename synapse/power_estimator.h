// ============================================================================
// synapse/power_estimator.h
// Project Synapse – Capstone Metric: Energy Impact
//
// Compile with -DSYNAPSE_POWER_VERIFY to enable post-session assertions that
// validate the model against expected physical bounds.  Never enable in a
// release driver build — assertions are for CI and lab measurement only.
// ============================================================================
#pragma once
#include "platform_config.h"   // pj_per_bit sourced from per-SKU config (Risk #3 fix)
#include "telemetry_types.h"   // canonical PowerReport — single source of truth (DRY fix)
#include <cassert>             // required by SYNAPSE_POWER_VERIFY assertions

namespace synapse::metrics {

class PowerEstimator {
public:
    /// @brief Energy constant sourced from PlatformConfig — not a compile-time literal.
    /// @return  Per-bit transfer energy (pJ) for the detected memory type.
    double pj_per_bit() const { return PlatformConfig::get().pj_per_bit; }

    /// @brief Accumulate energy for one memory transaction.
    ///
    /// @param full_bytes    Bytes that would have been transferred without ITS optimisation.
    /// @param actual_bytes  Bytes actually transferred (delta-compressed or prefetched).
    void log_transaction(uint64_t full_bytes, uint64_t actual_bytes) {
        const double ppb = pj_per_bit();
        total_potential_energy_pj_ += (full_bytes  * 8 * ppb);
        total_actual_energy_pj_    += (actual_bytes * 8 * ppb);
    }

    /// @brief Add the energy cost of a single P-State switch to the running tally.
    /// @param overhead_nj  Switch overhead from DVFSController::kSwitchOverheadNj (nJ).
    void log_switch_overhead(double overhead_nj) {
        total_actual_energy_pj_ += overhead_nj * 1000.0; // nJ -> pJ
    }

    // PowerReport is the canonical synapse::telemetry::PowerReport struct from
    // telemetry_types.h.  Bringing it into this namespace with a using-declaration
    // keeps call sites that use PowerEstimator::PowerReport unchanged.
    using PowerReport = synapse::telemetry::PowerReport;

    /// @brief Compute the session energy savings report dynamically from logged data.
    /// @return synapse::telemetry::PowerReport populated with computed values.
    PowerReport generate() const {
        double saved_pj = total_potential_energy_pj_ - total_actual_energy_pj_;
        double saved_j  = saved_pj / 1e12; // pJ -> J

        // Power (W) = Energy (J) / Time (s); at 60 FPS each frame = 1/60 s.
        const double frame_time_s = 1.0 / 60.0;
        double mw_saved = (total_frames_ > 0)
            ? (saved_j / (total_frames_ * frame_time_s)) * 1000.0
            : 0.0;

        const double extension = (total_potential_energy_pj_ > 0)
            ? saved_pj / total_potential_energy_pj_
            : 0.0;

        return { saved_j, mw_saved, extension };
    }

    // -----------------------------------------------------------------------
    /// @brief Validation gate — enabled only when SYNAPSE_POWER_VERIFY is defined.
    ///
    /// Asserts that:
    ///   1. joules_saved > 0  (ITS actually reduced bandwidth)
    ///   2. battery_extension_factor is in [0, 1]  (sanity range)
    ///   3. avg_milliwatts_saved_at_60fps < 5000.0  (not physically absurd)
    ///
    /// Call this at session end in CI / lab builds to catch model drift.
    // -----------------------------------------------------------------------
    void verify() const {
#ifdef SYNAPSE_POWER_VERIFY
        const PowerReport r = generate();
        // Rule 1: any ITS-active session must save positive energy
        assert(r.joules_saved > 0.0 &&
               "SYNAPSE_POWER_VERIFY: joules_saved must be > 0 for an ITS-active session");
        // Rule 2: extension factor must be a valid ratio
        assert(r.battery_extension_factor >= 0.0 &&
               r.battery_extension_factor <= 1.0 &&
               "SYNAPSE_POWER_VERIFY: battery_extension_factor out of [0,1] range");
        // Rule 3: savings must be physically plausible (< 5 W average)
        assert(r.avg_milliwatts_saved_at_60fps < 5000.0 &&
               "SYNAPSE_POWER_VERIFY: mW savings exceed physical plausibility bound");
#endif // SYNAPSE_POWER_VERIFY
    }

    /// @brief Advance the frame counter. Call once per rendered frame.
    void increment_frame() { total_frames_++; }

private:
    uint64_t total_frames_              = 0;
    double   total_potential_energy_pj_ = 0;
    double   total_actual_energy_pj_    = 0;
};

} // namespace synapse::metrics