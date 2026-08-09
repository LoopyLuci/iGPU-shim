// ============================================================================
// synapse/personal/privacy_telemetry.h
// Project Synapse – Privacy-First Telemetry with Differential Privacy
//
// Data classification: Public, Personal, Sensitive, Derived.
// Laplace noise added to non-public data. Budget tracking prevents
// privacy exhaustion. Local-only storage by default.
// ============================================================================
#pragma once

#include <cstdint>
#include <cmath>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace synapse::personal {

// Data classification levels
enum class DataClass : uint32_t {
    Public   = 0,  // Safe to collect without consent
    Personal = 1,  // Collect with consent
    Sensitive = 2, // Never collect
    Derived  = 3   // Computed, not raw
};

// Differential privacy with Laplace mechanism
class DifferentialPrivacy {
public:
    explicit DifferentialPrivacy(double epsilon = 1.0)
        : epsilon_(epsilon)
        , budget_remaining_(epsilon)
        , rng_(std::random_device{}()) {}

    // Add calibrated noise based on data classification
    template<typename T>
    T privatize(T value, DataClass classification) {
        if (classification == DataClass::Public)   return value;
        if (classification == DataClass::Sensitive) return T{};

        double sensitivity = 1.0;  // L1 sensitivity
        double scale = sensitivity / epsilon_;
        double noise = laplace_noise(scale);

        budget_remaining_ -= sensitivity / epsilon_;
        return static_cast<T>(value + noise);
    }

    // Check if privacy budget remains
    bool budget_remaining() const { return budget_remaining_ > 0.01; }
    double remaining_budget() const { return budget_remaining_; }

private:
    double epsilon_;
    double budget_remaining_;
    mutable std::mutex mutex_;
    std::mt19937 rng_;

    double laplace_noise(double scale) {
        std::lock_guard lock(mutex_);
        std::exponential_distribution<double> dist(1.0 / scale);
        double u = dist(rng_) - dist(rng_);  // Symmetric
        return u;
    }
};

// Local-only telemetry storage with consent management
class LocalStorage {
public:
    explicit LocalStorage(const std::string& data_dir)
        : data_dir_(data_dir) {}

    // Store telemetry entry locally
    bool store(const std::vector<uint8_t>& data, DataClass classification) {
        if (classification == DataClass::Sensitive) return false;

        std::lock_guard lock(mutex_);
        entries_.push_back({data, classification});
        return true;
    }

    // Export data with explicit consent check
    std::vector<std::vector<uint8_t>> export_data(const std::string& purpose) const {
        std::lock_guard lock(mutex_);
        if (!has_consent(purpose)) return {};

        std::vector<std::vector<uint8_t>> result;
        for (const auto& e : entries_) {
            if (e.classification != DataClass::Sensitive) {
                result.push_back(e.data);
            }
        }
        return result;
    }

    // Consent management
    bool has_consent(const std::string& purpose) const {
        std::lock_guard lock(mutex_);
        return consents_.count(purpose) > 0 && consents_.at(purpose);
    }

    void grant_consent(const std::string& purpose) {
        std::lock_guard lock(mutex_);
        consents_[purpose] = true;
    }

    void revoke_consent(const std::string& purpose) {
        std::lock_guard lock(mutex_);
        consents_[purpose] = false;
    }

    size_t entry_count() const {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }

private:
    struct Entry {
        std::vector<uint8_t> data;
        DataClass classification;
    };

    std::string data_dir_;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::map<std::string, bool> consents_;
};

}  // namespace synapse::personal
