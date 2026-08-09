// ============================================================================
// synapse/atomic/graceful_degradation.h
// Project Synapse – Feature Flags with Automatic Degradation
//
// Each feature has an operational state: Enabled, Degraded, Disabled, or Fallback.
// On error, features automatically degrade to the appropriate state.
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace synapse::atomic {

// Feature operational states
enum class FeatureState : uint32_t {
    Enabled  = 0,  // Full functionality
    Degraded = 1,  // Reduced functionality
    Disabled = 2,  // Feature unavailable
    Fallback = 3   // Using alternative implementation
};

// Feature flag manager with automatic degradation
class GracefulDegradation {
public:
    // Register a feature with its initial state
    void register_feature(const std::string& name,
                          FeatureState initial = FeatureState::Enabled) {
        features_[name] = initial;
        initial_states_[name] = initial;
    }

    // Get current state of a feature
    FeatureState state(const std::string& name) const {
        auto it = features_.find(name);
        if (it == features_.end()) return FeatureState::Disabled;
        return it->second;
    }

    // Check if a feature is usable (Enabled or Degraded)
    bool is_available(const std::string& name) const {
        auto s = state(name);
        return s == FeatureState::Enabled || s == FeatureState::Degraded;
    }

    // Handle error for a feature
    // error_code: 0=recovery, 1=transient(degrade), 2=persistent(disable), 3=fallback
    void handle_error(const std::string& name, uint32_t error_code) {
        auto& current = features_[name];

        switch (error_code) {
            case 0:  // Recovery
                current = initial_states_.count(name)
                    ? initial_states_[name]
                    : FeatureState::Enabled;
                break;
            case 1:  // Transient error → degrade
                current = FeatureState::Degraded;
                break;
            case 2:  // Persistent error → disable
                current = FeatureState::Disabled;
                break;
            case 3:  // Alternative available → fallback
                current = FeatureState::Fallback;
                break;
            default:
                current = FeatureState::Disabled;
                break;
        }
    }

    // Reset all features to initial state
    void reset_all() {
        for (const auto& [name, initial] : initial_states_) {
            features_[name] = initial;
        }
    }

    // Summary of all feature states
    struct Summary {
        uint32_t enabled{0};
        uint32_t degraded{0};
        uint32_t disabled{0};
        uint32_t fallback{0};
    };

    Summary summary() const {
        Summary s{};
        for (const auto& [name, st] : features_) {
            switch (st) {
                case FeatureState::Enabled:  s.enabled++;  break;
                case FeatureState::Degraded: s.degraded++; break;
                case FeatureState::Disabled: s.disabled++; break;
                case FeatureState::Fallback: s.fallback++; break;
            }
        }
        return s;
    }

    // Number of registered features
    size_t feature_count() const { return features_.size(); }

private:
    std::unordered_map<std::string, FeatureState> features_;
    std::unordered_map<std::string, FeatureState> initial_states_;
};

}  // namespace synapse::atomic
