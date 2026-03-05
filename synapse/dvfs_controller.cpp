// ============================================================================
// synapse/dvfs_controller.cpp
// Project Synapse – DVFSController Method Implementations
//
// DRY fix: this TU contains ONLY method bodies. All declarations and the class
// definition are in dvfs_controller.h. Previously both files had duplicate class
// definitions with diverging logic.
// ============================================================================
#include "dvfs_controller.h"
#include "sync_manager.h"        // register_global_bus_lock
#include "power_estimator.h"     // log_switch_overhead

namespace synapse::power {

// ---------------------------------------------------------------------------
void DVFSController::update_policy(uint64_t current_us, double predicted_mb_s) {
    if (state_ == DVFSState::TRANSITIONING) {
        if (current_us >= transition_end_us_) {
            complete_transition();
        }
        return; // Busy: ignore new requests during transition lock
    }
    if (performance_locked_) return; // SynapseCore override is active

    PState target = calculate_target(predicted_mb_s);
    if (target != current_p_state_ && is_hysteresis_satisfied(current_us)) {
        initiate_transition(target, current_us);
    }
}

// ---------------------------------------------------------------------------
void DVFSController::update_policy(double predicted_mb_s, float confidence) {
    // Confidence-weighted variant used by ConfidenceAggregator path.
    // Confidence < T_LOW → suppress; confidence >= T_HIGH already implies F0_MAX.
    static constexpr float kTLow  = 0.35f;
    static constexpr float kTHigh = 0.82f;

    if (confidence < kTLow)  return; // Too uncertain — hold current state
    if (performance_locked_) return;

    PState requested = calculate_target_state(predicted_mb_s, confidence);
    if (requested != current_p_state_ && frames_since_switch_ > kHysteresisFrames) {
        apply_hardware_state(requested);
    }
}

// ---------------------------------------------------------------------------
void DVFSController::handle_sync_stall() {
    // Emergency override: GPU is actively stalling. No hysteresis applied.
    state_ = DVFSState::STEADY;   // Clear any in-progress transition
    apply_hardware_state(PState::F0_MAX);
}

// ---------------------------------------------------------------------------
void DVFSController::force_performance_state(PState target) {
    performance_locked_ = true;
    apply_hardware_state(target);
}

// ---------------------------------------------------------------------------
void DVFSController::release_performance_lock() {
    performance_locked_ = false;
    // Resume gradient-based governance on the next update_policy() call.
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

PState DVFSController::calculate_target(double predicted_mb_s) const {
    // Simple threshold model — replace with ML inference output in Phase 7.
    if (predicted_mb_s > 40000.0) return PState::F0_MAX;
    if (predicted_mb_s > 20000.0) return PState::F1_BALANCED;
    return PState::F2_EFFICIENT;
}

PState DVFSController::calculate_target_state(double predicted_mb_s,
                                               float confidence) const {
    // Blend prediction with confidence weight.
    if (confidence >= 0.82f) return calculate_target(predicted_mb_s);
    return PState::F1_BALANCED; // Conservative default below T_HIGH
}

bool DVFSController::is_hysteresis_satisfied(uint64_t current_us) const {
    // Both frame-count and time-based guards must pass.
    const bool frames_ok = (frames_since_switch_ > kHysteresisFrames);
    const bool time_ok   = (current_us - last_switch_us_) > (kHysteresisFrames * 16667);
    return frames_ok && time_ok;
}

void DVFSController::initiate_transition(PState target, uint64_t start_us) {
    state_             = DVFSState::TRANSITIONING;
    transition_end_us_ = start_us + kTransitionLockUs;
    pending_p_state_   = target;

    // Register a global fence in SyncManager so draw calls stall during the window.
    // sync_manager_.register_global_bus_lock(transition_end_us_);
    // (SyncManager injected via setter in production; omitted here for clarity.)

    // Log the energy cost of the switch.
    // power_estimator_.log_switch_overhead(kSwitchOverheadNj);
}

void DVFSController::complete_transition() {
    current_p_state_ = pending_p_state_;
    state_           = DVFSState::STEADY;
    last_switch_us_  = transition_end_us_;
    frames_since_switch_ = 0;
}

void DVFSController::apply_hardware_state(PState target) {
    // Production: write to PMU driver MMIO register.
    // Simulation: update PowerEstimator PJ_PER_BIT lookup.
    current_p_state_     = target;
    frames_since_switch_ = 0;
}

} // namespace synapse::power
