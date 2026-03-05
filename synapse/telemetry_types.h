// ============================================================================
// synapse/telemetry_types.h
// Project Synapse – Canonical Telemetry Structures (Single Source of Truth)
//
// Previously stats structs were scattered across individual module headers.
// All reporting code and test harnesses MUST use these definitions.
// DRY fix: consolidates PredictionStats, HorizonStats, DVFSStats, JITStats,
//          HAIStats, ThermalStats, and the top-level SynapseSessionReport.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace synapse::telemetry {

// ----------------------------------------------------------------------------
// ITS / PredictiveEngine
// ----------------------------------------------------------------------------
struct PredictionStats {
    uint64_t total_predictions   = 0;
    uint64_t accurate_predictions= 0;
    uint64_t wasted_predictions  = 0;

    double accuracy_rate() const {
        return total_predictions > 0
            ? static_cast<double>(accurate_predictions) / total_predictions
            : 0.0;
    }
    double waste_rate() const {
        return total_predictions > 0
            ? static_cast<double>(wasted_predictions) / total_predictions
            : 0.0;
    }
};

// ----------------------------------------------------------------------------
// ITSCacheController
// ----------------------------------------------------------------------------
struct CacheMetrics {
    uint64_t hits         = 0;
    uint64_t misses       = 0;
    uint32_t sync_stalls  = 0;     ///< DMA fences not signaled in time
    uint64_t current_usage_bytes = 0;

    float hit_rate() const {
        const uint64_t total = hits + misses;
        return total > 0 ? static_cast<float>(hits) / static_cast<float>(total) : 0.0f;
    }
};

// ----------------------------------------------------------------------------
// ForecastingProfiler — one entry per evaluated horizon window
// ----------------------------------------------------------------------------
struct HorizonStats {
    uint32_t window          = 0;  ///< Frames looked ahead
    uint32_t stalls_avoided  = 0;
    uint32_t false_positives = 0;  ///< Wasted transitions

    double energy_efficiency() const {
        const uint32_t total = stalls_avoided + false_positives;
        return total > 0
            ? static_cast<double>(stalls_avoided) / total
            : 0.0;
    }
};

// ----------------------------------------------------------------------------
// DVFSController
// ----------------------------------------------------------------------------
struct DVFSStats {
    uint32_t total_transitions     = 0;
    uint32_t emergency_overrides   = 0;  ///< handle_sync_stall() calls
    uint32_t hysteresis_drops      = 0;  ///< Requests dropped by hysteresis
    double   total_switch_energy_nj= 0.0;
};

// ----------------------------------------------------------------------------
// JITPipeline / SynapseCore stutter tracking
// ----------------------------------------------------------------------------
struct JITStats {
    uint32_t cold_cache_fallbacks = 0;
    uint32_t cache_hits           = 0;
    double   worst_fallback_ms    = 0.0;
    double   total_fallback_ms    = 0.0;
    static constexpr double kBudgetMs = 2.0;

    bool is_over_stutter_budget() const { return worst_fallback_ms > kBudgetMs; }
};

// ----------------------------------------------------------------------------
// HAI bytecode path
// ----------------------------------------------------------------------------
struct HAIStats {
    uint64_t full_draws_emitted  = 0;
    uint64_t delta_draws_emitted = 0;
    uint64_t raw_bytes_equivalent= 0;  ///< What full encoding would have cost
    uint64_t actual_bytes_emitted= 0;

    double compression_ratio() const {
        return actual_bytes_emitted > 0
            ? static_cast<double>(raw_bytes_equivalent) / actual_bytes_emitted
            : 1.0;
    }
};

// ----------------------------------------------------------------------------
// Thermal / PGRO
// ----------------------------------------------------------------------------
struct ThermalStats {
    uint32_t thermal_mitigation_events = 0;
    uint32_t stability_overrides       = 0;  ///< resolve_power_perf_conflict()
    uint32_t proactive_boosts          = 0;  ///< SmoothingEngine SET_EXPECTED_LOAD
};

// ----------------------------------------------------------------------------
// PowerEstimator
// ----------------------------------------------------------------------------
struct PowerReport {
    double joules_saved                  = 0.0;
    double avg_milliwatts_saved_at_60fps = 0.0;
    double battery_extension_factor      = 0.0;  ///< E_saved / E_potential
};

// ----------------------------------------------------------------------------
// Top-level session report — mirrors report.json schema
// ----------------------------------------------------------------------------
struct SynapseSessionReport {
    std::string heuristic       = "temporal_locality_v1";
    uint32_t    temporal_window_frames = 3;

    PredictionStats its_predictor;
    CacheMetrics    its_cache;
    HorizonStats    best_horizon;   ///< Horizon with highest energy_efficiency()
    DVFSStats       dvfs;
    JITStats        jit;
    HAIStats        hai;
    ThermalStats    thermal;
    PowerReport     power;
};

} // namespace synapse::telemetry
