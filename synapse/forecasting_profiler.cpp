// ============================================================================
// synapse/forecasting_profiler.cpp
// Project Synapse – Phase 4 Part B: Horizon Analysis
//
// Evaluates four look-ahead windows (5/10/20/30 frames) and selects the one
// that maximises energy efficiency.  Results are serialised directly into the
// top-level SynapseSessionReport so they populate report.json at session end.
// ============================================================================
#include "telemetry_types.h"   // HorizonStats, SynapseSessionReport

#include <algorithm>
#include <vector>

namespace synapse::its {

// ---------------------------------------------------------------------------
// ForecastingProfiler
// ---------------------------------------------------------------------------
class ForecastingProfiler {
public:
    // -----------------------------------------------------------------------
    /// @brief Evaluate all horizon windows for the current frame.
    ///
    /// For each window W, asks: "If we had issued a P-State ramp W frames
    /// ago, would we have avoided the stall observed this frame?"
    ///
    /// @param current_f  Monotonically-increasing frame counter.
    /// @param sig        WorkloadSignature captured this frame.
    // -----------------------------------------------------------------------
    void analyze_frame(uint64_t current_f, const WorkloadSignature& sig) {
        for (auto& horizon : horizons_) {
            if (is_approaching_heavy_workload(current_f, horizon.window)) {
                if (validate_prediction(current_f + horizon.window)) {
                    horizon.stalls_avoided++;
                } else {
                    horizon.false_positives++;
                }
            }
        }
        total_frames_analyzed_++;
    }

    // -----------------------------------------------------------------------
    /// @brief Return the horizon entry with the highest energy_efficiency().
    ///
    /// Ties are broken by preferring the shorter (lower-latency) window.
    /// @return  Best HorizonStats by efficiency. Returns window=0 if empty.
    // -----------------------------------------------------------------------
    synapse::telemetry::HorizonStats get_best_horizon() const {
        synapse::telemetry::HorizonStats best{};
        double best_eff = -1.0;
        for (const auto& h : horizons_) {
            const double eff = h.energy_efficiency();
            if (eff > best_eff) {
                best_eff = eff;
                best     = h;
            }
        }
        return best;
    }

    // -----------------------------------------------------------------------
    /// @brief Return all four horizon entries (windows 5, 10, 20, 30).
    // -----------------------------------------------------------------------
    const std::vector<synapse::telemetry::HorizonStats>& all_horizons() const {
        return horizons_;
    }

    /// @brief Total frames that have passed through analyze_frame().
    uint64_t total_frames_analyzed() const { return total_frames_analyzed_; }

    // -----------------------------------------------------------------------
    /// @brief Serialise horizon analysis results into a SynapseSessionReport.
    ///
    /// Called once per session to populate the report.json horizon_analysis
    /// section. The best horizon is stored in report.best_horizon; the full
    /// per-window breakdown is written to report.horizon_windows[].
    ///
    /// @param[out] report  Top-level session report to populate.
    // -----------------------------------------------------------------------
    void serialize_to_report(synapse::telemetry::SynapseSessionReport& report) const {
        report.best_horizon           = get_best_horizon();
        report.horizon_windows        = horizons_;          // all four window entries
        report.total_frames_analyzed  = total_frames_analyzed_;
    }

private:
    // -----------------------------------------------------------------------
    // Helpers (stubs — in production these query the telemetry ring buffer)
    // -----------------------------------------------------------------------

    /// @brief Returns true if workload complexity trend signals a looming stall.
    bool is_approaching_heavy_workload(uint64_t /*frame*/, uint32_t /*lookahead*/) const {
        // Production: compare rolling avg shader_instruction_estimate with threshold
        return false;
    }

    /// @brief Returns true if the predicted stall did occur at the target frame.
    bool validate_prediction(uint64_t /*target_frame*/) const {
        // Production: walk historical telemetry ring buffer entries
        return false;
    }

    // Four canonical look-ahead windows
    std::vector<synapse::telemetry::HorizonStats> horizons_ = {
        {5}, {10}, {20}, {30}
    };

    uint64_t total_frames_analyzed_ = 0;
};

} // namespace synapse::its