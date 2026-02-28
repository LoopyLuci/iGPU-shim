// ============================================================================
// synapse/pgro_smoothing_engine.cpp
// Project Synapse – Phase 5: Compute-Aware Stability
// ============================================================================
void SmoothingEngine::process_forecast(const Forecast& f) {
    float shader_trend = calculate_shader_complexity_trend(f);
    
    // If we anticipate a significant compute spike...
    if (shader_trend > COMPLEXITY_THRESHOLD && f.confidence > 0.82f) {
        
        // Emit 0x50 SET_EXPECTED_LOAD
        // This hint tells the scheduler to ramp up shader clocks
        // and increase thread occupancy limits for upcoming frames.
        uint32_t expected_cycles = estimate_total_cycles(f);
        builder_.emit_scheduler_hint(Priority::HIGH, expected_cycles);
        
        stats_.proactive_boosts++;
    }
}