// ============================================================================
// synapse/its_confidence_aggregator.h
// Project Synapse – Phase 4 Part B: Probabilistic Forecasting
// ============================================================================
namespace synapse::its {

class ConfidenceAggregator {
public:
    struct Weights {
        float velocity = 0.4f;
        float mip_gradient = 0.4f;
        float historical = 0.2f;
    };

    float compute_composite_score(float v_score, float m_score, float h_score) {
        float total = (v_score * weights_.velocity) + 
                      (m_score * weights_.mip_gradient) + 
                      (h_score * weights_.historical);
                      
        // Defensive: Clamp output to [0.0, 1.0]
        return std::clamp(total, 0.0f, 1.0f);
    }

    // Thresholds for DVFS Controller
    static constexpr float T_HIGH = 0.82f; // Trigger Ramp-up
    static constexpr float T_LOW  = 0.35f; // Suppress/Cancel

private:
    Weights weights_;
};

} // namespace synapse::its