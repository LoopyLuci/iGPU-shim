// ============================================================================
// synapse/pgro_smoothing_engine.h
// Project Synapse – Phase 5: Compute-Aware Stability
// ============================================================================
#pragma once

#include "telemetry_types.h"
#include "synapse_hai_builder.h"
#include <cstdint>

namespace synapse {

// Simple telemetry stats for the smoothing engine
struct SmoothingTelemetryStats {
    uint32_t proactive_boosts = 0;
};

struct Forecast {
    float confidence = 0.0f;
    uint32_t estimated_cycles = 0;
};

namespace builder { class HAIBytecodeBuilder; }

class SmoothingEngine {
public:
    SmoothingEngine(builder::HAIBytecodeBuilder& builder) : builder_(builder) {}

    void process_forecast(const Forecast& f);
    void suppress_boosts(bool suppress) { suppressed_ = suppress; }
    bool is_stability_critical() const { return stability_critical_; }
    bool is_suppressed() const { return suppressed_; }

private:
    float calculate_shader_complexity_trend(const Forecast& f) const;
    uint32_t estimate_total_cycles(const Forecast& f) const;

    static constexpr float COMPLEXITY_THRESHOLD = 0.75f;

    builder::HAIBytecodeBuilder& builder_;
    SmoothingTelemetryStats stats_;
    bool suppressed_ = false;
    bool stability_critical_ = false;
};

} // namespace synapse
