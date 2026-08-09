// ============================================================================
// synapse/thermal_aware_arbiter.h
// Project Synapse – Phase 5 Part B: Environmental Hardening
// ============================================================================
#pragma once

#include "platform_config.h"
#include "telemetry_types.h"
#include <cstdint>

namespace synapse {

// Forward declarations
class SmoothingEngine;
namespace its { class PredictiveEngine; }

// Simple telemetry stats for the arbiter
struct TelemetryStats {
    uint32_t thermal_mitigation_events = 0;
    uint32_t proactive_boosts = 0;
};

class PolicyArbiter {
public:
    enum class Mode { STANDARD, THERMAL_MITIGATION };

    PolicyArbiter(SmoothingEngine& smoothing, its::PredictiveEngine& predictive);

    void resolve_environmental_state(float thermal_headroom);
    Mode current_mode() const { return current_mode_; }
    const TelemetryStats& stats() const { return stats_; }

private:
    SmoothingEngine& smoothing_engine_;
    its::PredictiveEngine& predictive_engine_;
    TelemetryStats stats_;
    Mode current_mode_ = Mode::STANDARD;
};

} // namespace synapse
