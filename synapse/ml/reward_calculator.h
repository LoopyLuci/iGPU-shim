#pragma once

#include "telemetry_types.h"
#include "power_estimator.h"

namespace synapse::ml {

struct RewardConfig {
    float frame_time_weight = 1.0f;
    float energy_saved_weight = 1.0f;
    float stall_penalty = -10.0f;
    float jit_stutter_penalty = -5.0f;
};

class RewardCalculator {
public:
    RewardCalculator() = default;

    // Compute a scalar reward from observed data. Keep simple and fast.
    float compute(float frame_time_ms,
                  const synapse::metrics::PowerEstimator::PowerReport& pr,
                  uint32_t stalls,
                  uint32_t jit_over_budget_count) const
    {
        float r = 0.0f;
        r -= frame_time_ms * cfg_.frame_time_weight;            // lower frame time => higher reward
        r += static_cast<float>(pr.joules_saved) * cfg_.energy_saved_weight;
        r += static_cast<float>(stalls) * cfg_.stall_penalty;
        r += static_cast<float>(jit_over_budget_count) * cfg_.jit_stutter_penalty;
        return r;
    }

    void set_config(const RewardConfig& c) { cfg_ = c; }

private:
    RewardConfig cfg_;
};

} // namespace synapse::ml
