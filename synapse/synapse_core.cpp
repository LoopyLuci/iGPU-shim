// ============================================================================
// synapse/synapse_core.cpp (Arbiter Logic)
// Project Synapse – Phase 5: Stability-First Priority
// ============================================================================
void SynapseCore::resolve_power_perf_conflict() {
    bool stability_at_risk = smoothing_engine_.is_stability_critical();
    
    if (stability_at_risk) {
        // High-Priority Override
        dvfs_controller_.force_performance_state(PState::F0_MAX);
        
        if (!was_previously_locked_) {
            stats_.stability_overrides_count++;
            was_previously_locked_ = true;
        }
    } else {
        // Resume standard predictive power governance
        dvfs_controller_.release_performance_lock();
        was_previously_locked_ = false;
    }
}