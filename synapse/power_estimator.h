// ============================================================================
// synapse/power_estimator.h
// Project Synapse – Capstone Metric: Energy Impact
// ============================================================================
namespace synapse::metrics {

class PowerEstimator {
public:
    static constexpr double PJ_PER_BIT = 35.0; // 2026 LPDDR5 Baseline

    void log_transaction(uint64_t full_bytes, uint64_t actual_bytes) {
        total_potential_energy_pj_ += (full_bytes * 8 * PJ_PER_BIT);
        total_actual_energy_pj_ += (actual_bytes * 8 * PJ_PER_BIT);
    }

    struct PowerReport {
        double joules_saved;
        double avg_milliwatts_saved_at_60fps;
        double battery_extension_factor; // Theoretical
    };

    PowerReport generate() const {
        double saved_pj = total_potential_energy_pj_ - total_actual_energy_pj_;
        double saved_j = saved_pj / 1e12; // Pico to Joules
        
        // 60 FPS = 0.0166s per frame. 
        // Power (W) = Energy (J) / Time (s)
        double mw_saved = (saved_j / (total_frames_ * 0.0166)) * 1000.0;

        return { saved_j, mw_saved, (saved_pj / total_potential_energy_pj_) };
    }

private:
    uint64_t total_frames_ = 0;
    double total_potential_energy_pj_ = 0;
    double total_actual_energy_pj_ = 0;
};

} // namespace synapse::metrics