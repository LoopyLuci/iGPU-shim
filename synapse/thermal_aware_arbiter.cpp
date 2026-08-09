// ============================================================================
// synapse/thermal_aware_arbiter.cpp
// Project Synapse – Phase 5 Part B: Environmental Hardening
// ============================================================================
#include "thermal_aware_arbiter.h"
#include "platform_config.h"
#include "pgro_smoothing_engine.h"
#include "predictive_engine.h"

using namespace synapse;

PolicyArbiter::PolicyArbiter(SmoothingEngine& smoothing, its::PredictiveEngine& predictive)
    : smoothing_engine_(smoothing), predictive_engine_(predictive) {}

void PolicyArbiter::resolve_environmental_state(float thermal_headroom) {
    const float threshold = PlatformConfig::get().thermal_mitigation_threshold;

    if (thermal_headroom < threshold) { // Thermal Mitigation Zone (SKU-configurable)
        current_mode_ = Mode::THERMAL_MITIGATION;
        
        // 1. Block PGRO from requesting F0
        smoothing_engine_.suppress_boosts(true);
        
        // 2. Command Predictive Engine to shed load
        // "Mip-Capping" – reduces texture detail to reclaim bandwidth
        predictive_engine_.set_mip_cap(2); 
        
        stats_.thermal_mitigation_events++;
    } else {
        current_mode_ = Mode::STANDARD;
        smoothing_engine_.suppress_boosts(false);
        predictive_engine_.clear_mip_cap();
    }
}
