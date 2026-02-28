// ============================================================================
// synapse/dvfs_controller.cpp
// Project Synapse – Phase 4: Latency-Aware Power Governance
// ============================================================================
namespace synapse::power {

enum class DVFSState { STEADY, TRANSITIONING };

class DVFSController {
public:
    void update_policy(uint64_t current_us, double predicted_mb_s) {
        if (state_ == DVFSState::TRANSITIONING) {
            if (current_us >= transition_end_us_) {
                complete_transition();
            }
            return; // Busy: Ignore new requests during lock
        }

        PState target = calculate_target(predicted_mb_s);
        if (target != current_p_state_ && is_hysteresis_satisfied(current_us)) {
            initiate_transition(target, current_us);
        }
    }

private:
    void initiate_transition(PState target, uint64_t start_us) {
        state_ = DVFSState::TRANSITIONING;
        transition_end_us_ = start_us + 75; // 75us Lock
        pending_p_state_ = target;

        // Register a Global Fence in the SyncManager.
        // Any draw call starting before transition_end_us_ MUST stall.
        sync_manager_.register_global_bus_lock(transition_end_us_);
        
        // Log the energy cost of the switch
        power_estimator_.log_switch_overhead(150); // 150nJ tax
    }

    void complete_transition() {
        current_p_state_ = pending_p_state_;
        state_ = DVFSState::STEADY;
        last_switch_us_ = transition_end_us_;
    }

    DVFSState state_ = DVFSState::STEADY;
    uint64_t transition_end_us_ = 0;
    uint64_t last_switch_us_ = 0;
};

} // namespace synapse::power