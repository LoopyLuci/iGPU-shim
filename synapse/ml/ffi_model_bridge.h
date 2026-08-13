#pragma once

#include "ffi/synapse_igpu_ml.h"
#include <string>
#include <memory>
#include <stdexcept>

// Declarations for the C FFI from the Rust static library.
extern "C" {
    unsigned long long synapse_ml_create_model(const char* name);
    int synapse_ml_destroy_model(unsigned long long handle);
    SynapseActionScores synapse_ml_infer(unsigned long long handle, const SynapseFeatureVector* features);
    SynapseUpdateResult synapse_ml_observe(unsigned long long handle, const SynapseExperience* experience);
    SynapseModelExplanation synapse_ml_explain(unsigned long long handle, const SynapseFeatureVector* features);
}

namespace synapse::ml {

class FfiModelBridge {
public:
    explicit FfiModelBridge(const std::string& model_name) {
        handle_ = synapse_ml_create_model(model_name.c_str());
        if (handle_ == UINT64_MAX) {
            throw std::runtime_error("Failed to create ML model: " + model_name);
        }
    }

    ~FfiModelBridge() {
        if (handle_ != UINT64_MAX) {
            synapse_ml_destroy_model(handle_);
        }
    }

    FfiModelBridge(const FfiModelBridge&) = delete;
    FfiModelBridge& operator=(const FfiModelBridge&) = delete;

    SynapseActionScores infer(const SynapseFeatureVector& features) const {
        return synapse_ml_infer(handle_, &features);
    }

    SynapseUpdateResult observe(const SynapseExperience& experience) const {
        return synapse_ml_observe(handle_, &experience);
    }

    SynapseModelExplanation explain(const SynapseFeatureVector& features) const {
        return synapse_ml_explain(handle_, &features);
    }

    unsigned long long handle() const { return handle_; }

private:
    unsigned long long handle_{UINT64_MAX};
};

} // namespace synapse::ml
