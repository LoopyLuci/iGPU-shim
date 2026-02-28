// ============================================================================
// synapse/dvfs_controller.h
// Project Synapse – Phase 4: Predictive Power Governance
// ============================================================================
namespace synapse::power {

enum class PState { F0_MAX, F1_BALANCED, F2_EFFICIENT };

class DVFSController {
public:
    /**
     * @brief Updates the target frequency based on the Predictive Engine's output.
     */
    void update_policy(double predicted_mb_s, float confidence) {
        PState requested = calculate_target_state(predicted_mb_s, confidence);
        
        // Defensive: Hysteresis check. Don't switch states if we just switched 
        // within the last 5 frames unless it's an emergency ramp-up.
        if (requested != current_state_ && frames_since_switch_ > 5) {
            apply_hardware_state(requested);
        }
    }

    void handle_sync_stall() {
        // Emergency Override: The predictor missed, and the GPU is stalling.
        // Immediately force maximum voltage/frequency.
        apply_hardware_state(PState::F0_MAX);
    }

private:
    PState current_state_ = PState::F0_MAX;
    uint32_t frames_since_switch_ = 0;
    
    void apply_hardware_state(PState state) {
        // In simulation, this updates the PowerEstimator's PJ_PER_BIT value.
        // In production, this writes to the PMU driver.
        current_state_ = state;
        frames_since_switch_ = 0;
    }
};

} // namespace synapse::power