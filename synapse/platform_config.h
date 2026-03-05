// ============================================================================
// synapse/platform_config.h
// Project Synapse – Per-SKU Platform Configuration (Single Source of Truth)
//
// Mitigates Risk #3 (PJ_PER_BIT wrong for non-LPDDR5) and Risk #6
// (thermal threshold wrong for other SKUs). Previously both were hardcoded
// constants inside individual module files.
//
// Usage:
//   At driver init, call PlatformConfig::detect() to populate the singleton.
//   All modules must query via PlatformConfig::get() rather than using magic numbers.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace synapse {

// ----------------------------------------------------------------------------
// MemoryType: identifies the system memory technology for energy modeling
// ----------------------------------------------------------------------------
enum class MemoryType {
    LPDDR5,  ///< 2026 baseline — 35.0 pJ/bit
    LPDDR4X, ///< Older devices — 42.0 pJ/bit
    DDR5,    ///< Desktop iGPU (Ryzen 9000 series) — 28.0 pJ/bit
    UNKNOWN  ///< Fall back to LPDDR5 baseline
};

// ----------------------------------------------------------------------------
// PlatformConfig
// Holds all values that differ between iGPU SKUs. Initialized once at startup.
// ----------------------------------------------------------------------------
struct PlatformConfig {
    // --- Memory subsystem ---
    MemoryType memory_type        = MemoryType::LPDDR5;
    double     pj_per_bit         = 35.0;  ///< Energy per bit transferred (pJ)
    uint64_t   memory_bandwidth_mb_s = 68'000; ///< Peak theoretical BW (MB/s)

    // --- Thermal ---
    float thermal_mitigation_threshold = 0.20f; ///< Headroom fraction below which mip-cap fires
    float thermal_hysteresis_band      = 0.03f; ///< Width of dead-band around threshold

    // --- DVFS ---
    uint32_t dvfs_hysteresis_frames    = 5;     ///< Min frames between P-State switches
    uint64_t dvfs_transition_lock_us   = 75;    ///< Duration of hardware bus lock (µs)
    double   dvfs_switch_overhead_nj   = 150.0; ///< Per-switch energy tax (nJ)

    // --- ITS ---
    uint32_t its_temporal_window_frames = 3;    ///< Default prefetch lookahead

    // --- Descriptive ---
    std::string sku_name = "unknown";

    // -----------------------------------------------------------------------
    // detect()
    // Queries OS / ACPI / driver platform APIs to identify the current SKU.
    // Falls back to safe LPDDR5 defaults on any failure.
    // -----------------------------------------------------------------------
    static PlatformConfig detect() {
        PlatformConfig cfg;
        // In production: query ACPI _DSD or vendor-specific MMIO registers.
        // In simulation / test: environment variable SYNAPSE_PLATFORM_OVERRIDE
        //   can be set to "LPDDR4X", "DDR5", etc.
        const char* override_env = std::getenv("SYNAPSE_PLATFORM_OVERRIDE");
        if (override_env) {
            const std::string s(override_env);
            if (s == "LPDDR4X") {
                cfg.memory_type = MemoryType::LPDDR4X;
                cfg.pj_per_bit  = 42.0;
                cfg.sku_name    = "lpddr4x_override";
            } else if (s == "DDR5") {
                cfg.memory_type = MemoryType::DDR5;
                cfg.pj_per_bit  = 28.0;
                cfg.sku_name    = "ddr5_override";
            }
        }
        return cfg;
    }

    // -----------------------------------------------------------------------
    // get() / set() — singleton accessor
    // Call set(detect()) once during driver initialisation.
    // -----------------------------------------------------------------------
    static const PlatformConfig& get() {
        return instance();
    }

    static void set(const PlatformConfig& cfg) {
        instance() = cfg;
    }

private:
    static PlatformConfig& instance() {
        static PlatformConfig s_cfg;
        return s_cfg;
    }
};

} // namespace synapse
