// ============================================================================
// synapse/dvfs_controller.h
// Project Synapse – Predictive & Latency-Aware Power Governance
//
// Single canonical header. dvfs_controller.cpp contains method implementations.
// DRY fix: previously PState logic was split across both files.
// ============================================================================
#pragma once

#include <cstdint>

namespace synapse::power {

// ----------------------------------------------------------------------------
// P-State: voltage / frequency operating points
// ----------------------------------------------------------------------------
enum class PState {
    F0_MAX,       ///< Maximum voltage/frequency — heavy geometry, compute shaders
    F1_BALANCED,  ///< Mid-range — mixed workload (default)
    F2_EFFICIENT  ///< Minimum voltage/frequency — UI rendering, idle frames
};

// ----------------------------------------------------------------------------
// DVFSState: internal state machine for transition safety
// ----------------------------------------------------------------------------
enum class DVFSState {
    STEADY,       ///< No transition in progress; requests are accepted
    TRANSITIONING ///< 75 µs lock active; requests are queued or dropped
};

// ----------------------------------------------------------------------------
// DVFSController
// Manages iGPU frequency/voltage transitions with:
//   – 5-frame hysteresis window to prevent oscillation
//   – 75 µs transition lock registered with SyncManager
//   – 150 nJ per-switch energy cost logged to PowerEstimator
//   – Emergency bypass via handle_sync_stall() for active GPU stalls
// ----------------------------------------------------------------------------
class DVFSController {
public:
    DVFSController() = default;

    /**
     * @brief Called once per frame. Evaluates predicted bandwidth demand and
     *        requests a P-State transition when confidence and hysteresis allow.
     * @param current_us  Current timestamp in microseconds.
     * @param predicted_mb_s  Predicted memory bandwidth (MB/s) from PredictiveEngine.
     */
    void update_policy(uint64_t current_us, double predicted_mb_s);

    /**
     * @brief Emergency override: immediately forces F0_MAX when the GPU is
     *        stalling. Bypasses hysteresis and transition lock.
     */
    void handle_sync_stall();

    /**
     * @brief Called by SynapseCore when ConfidenceAggregator supplies a score.
     *        High-confidence signal path; respects hysteresis.
     * @param predicted_mb_s  Predicted demand.
     * @param confidence      Composite score from ConfidenceAggregator [0,1].
     */
    void update_policy(double predicted_mb_s, float confidence);

    /**
     * @brief Forces a specific P-State immediately, bypassing hysteresis.
     *        Used by SynapseCore::resolve_power_perf_conflict().
     */
    void force_performance_state(PState target);

    /**
     * @brief Releases a previously forced performance lock, resuming
     *        standard predictive governance.
     */
    void release_performance_lock();

    /// @brief Returns the currently active P-State.
    PState current_state() const { return current_p_state_; }

    /// @brief Returns true if a transition is in progress.
    bool is_transitioning() const { return state_ == DVFSState::TRANSITIONING; }

    static constexpr uint64_t kTransitionLockUs  = 75;   ///< 75 µs lock window
    static constexpr double   kSwitchOverheadNj  = 150.0; ///< Energy cost per switch
    static constexpr uint32_t kHysteresisFrames  = 5;    ///< Min frames between switches

private:
    PState     current_p_state_  = PState::F0_MAX;
    PState     pending_p_state_  = PState::F0_MAX;
    DVFSState  state_            = DVFSState::STEADY;
    uint64_t   transition_end_us_= 0;
    uint64_t   last_switch_us_   = 0;
    uint32_t   frames_since_switch_ = 0;
    bool       performance_locked_  = false;

    PState     calculate_target(double predicted_mb_s) const;
    PState     calculate_target_state(double predicted_mb_s, float confidence) const;
    bool       is_hysteresis_satisfied(uint64_t current_us) const;
    void       initiate_transition(PState target, uint64_t start_us);
    void       complete_transition();
    void       apply_hardware_state(PState target);
};

} // namespace synapse::power
