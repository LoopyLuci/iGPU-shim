// ============================================================================
// synapse/personal/user_profile.h
// Project Synapse – User Profile with Adaptive Optimization
//
// Learns usage patterns and adapts power/thermal/ML settings.
// Profiles are exported/imported for persistence.
// Supports time-of-day adaptation and preference overrides.
// ============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace synapse::personal {

// Usage statistics collected over time
struct UsageStats {
    uint32_t daily_usage_hours{0};
    bool     prefers_battery_life{false};
    bool     prefers_silence{false};
    uint32_t peak_hour{12};           // 0-23
    double   avg_frame_time_ms{16.7}; // 60 fps default
};

// User preferences (persisted)
struct Preferences {
    bool     auto_optimize{true};
    bool     show_notifications{true};
    bool     log_telemetry{true};
    uint32_t max_power_watts{15};
    std::string performance_profile{"balanced"};
};

// Optimization plan derived from usage + preferences
struct OptimizationPlan {
    uint32_t    power_budget_watts{15};
    uint32_t    thermal_target_celsius{80};
    bool        ml_aggressive{true};
    std::string fan_curve{"balanced"};
};

// Preset profile for quick setup
struct PresetProfile {
    const char* name;
    const char* description;
    uint32_t    power_budget;
    uint32_t    thermal_target;
    bool        ml_aggressive;
    const char* fan_curve;
};

static constexpr PresetProfile kPresets[] = {
    {"battery-saver", "Maximize battery life",          8,  70, false, "silent"},
    {"balanced",      "Good performance and battery",  15,  80, true,  "balanced"},
    {"performance",   "Maximum performance",           25,  85, true,  "performance"},
    {"silent",        "Minimize fan noise",            10,  65, false, "silent"},
};

class UserProfile {
public:
    UserProfile() = default;

    // Update usage statistics
    void update_usage(const UsageStats& stats) {
        usage_ = stats;
        recompute_plan();
    }

    // Update preferences
    void set_preferences(const Preferences& prefs) {
        prefs_ = prefs;
        recompute_plan();
    }

    // Get current plan
    const OptimizationPlan& plan() const { return plan_; }

    // Apply a preset profile
    void apply_preset(const char* name) {
        for (const auto& p : kPresets) {
            if (std::string(p.name) == name) {
                plan_.power_budget_watts  = p.power_budget;
                plan_.thermal_target_celsius = p.thermal_target;
                plan_.ml_aggressive = p.ml_aggressive;
                plan_.fan_curve = p.fan_curve;
                prefs_.performance_profile = name;
                return;
            }
        }
    }

    // Export for persistence
    struct ProfileData {
        UsageStats      usage;
        Preferences     prefs;
        OptimizationPlan plan;
    };

    ProfileData export_profile() const {
        return {usage_, prefs_, plan_};
    }

    // Import from persistence
    void import_profile(const ProfileData& data) {
        usage_ = data.usage;
        prefs_ = data.prefs;
        plan_ = data.plan;
    }

    const Preferences& preferences() const { return prefs_; }

private:
    UsageStats      usage_;
    Preferences     prefs_;
    OptimizationPlan plan_;

    void recompute_plan() {
        // Start from defaults
        plan_.power_budget_watts   = prefs_.max_power_watts;
        plan_.thermal_target_celsius = 80;
        plan_.ml_aggressive        = true;
        plan_.fan_curve            = "balanced";

        // Battery-first for portable use
        if (usage_.prefers_battery_life) {
            plan_.power_budget_watts = 8;
            plan_.thermal_target_celsius = 75;
            plan_.ml_aggressive = false;
            plan_.fan_curve = "silent";
        }

        // Heavy usage → performance
        if (usage_.daily_usage_hours > 8) {
            plan_.power_budget_watts = 25;
            plan_.thermal_target_celsius = 85;
            plan_.ml_aggressive = true;
            plan_.fan_curve = "performance";
        }

        // Night mode (22:00–06:00) → silence
        auto hour = std::chrono::system_clock::now().time_since_epoch();
        auto h = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::hours>(hour).count() % 24);
        if (h >= 22 || h < 6) {
            plan_.fan_curve = "silent";
            plan_.power_budget_watts =
                static_cast<uint32_t>(plan_.power_budget_watts * 7 / 10);
        }

        // User override
        if (!prefs_.auto_optimize) {
            plan_.power_budget_watts = prefs_.max_power_watts;
            plan_.fan_curve = prefs_.performance_profile;
        }
    }
};

}  // namespace synapse::personal
