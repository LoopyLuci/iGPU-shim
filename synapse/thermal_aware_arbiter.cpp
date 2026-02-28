// ============================================================================
// synapse/thermal_aware_arbiter.cpp
// Project Synapse – Phase 5 Part B: Environmental Hardening
// ============================================================================
void PolicyArbiter::resolve_environmental_state(float thermal_headroom) {
    if (thermal_headroom < 0.20f) { // Thermal Mitigation Zone
        current_mode_ = Mode::THERMAL_MITIGATION;
        
        // 1. Block PGRO from requesting F0
        smoothing_engine_.suppress_boosts(true);
        
        // 2. Command Predictive Engine to shed load
        // This is "Mip-Capping" - dropping detail to save the frame-rate
        predictive_engine_.set_mip_cap(2); 
        
        stats_.thermal_mitigation_events++;
    } else {
        current_mode_ = Mode::STANDARD;
        smoothing_engine_.suppress_boosts(false);
        predictive_engine_.clear_mip_cap();
    }
}