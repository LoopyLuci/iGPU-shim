#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct SynapseFeatureVector {
    float shader_complexity_norm;
    float vertex_count_log_norm;
    float draw_call_rate_norm;
    float cache_hit_rate;
    float dvfs_headroom;
    float thermal_headroom;
    float predictor_accuracy;
    float is_compute_dispatch;
} SynapseFeatureVector;

typedef struct SynapseExperience {
    SynapseFeatureVector features;
    uint32_t action;
    float reward;
    uint64_t timestamp_ns;
    uint64_t session_id;
    float energy_joules;
    uint32_t thermal_events;
    float confidence;
} SynapseExperience;

typedef struct SynapseActionScores {
    float scores[3];
    uint32_t chosen;
    float confidence;
    int epsilon_greedy;
} SynapseActionScores;

typedef struct SynapseModelExplanation {
    float feature_importances[8];
    float predicted_scores[3];
    float confidence;
    const char* model_version;
    const char* notes;
} SynapseModelExplanation;

typedef struct SynapseUpdateResult {
    float loss;
    float gradient_norm;
    float learning_rate;
} SynapseUpdateResult;

uint64_t synapse_ml_create_model(const char* name) { return 1; }
int synapse_ml_destroy_model(uint64_t handle) { return 0; }
SynapseActionScores synapse_ml_infer(uint64_t handle, const SynapseFeatureVector* fv) {
    SynapseActionScores r = {{0.4f, 0.4f, 0.2f}, 0u, 0.8f, 0};
    return r;
}
SynapseUpdateResult synapse_ml_observe(uint64_t handle, const SynapseExperience* exp) {
    (void)handle; (void)exp;
    SynapseUpdateResult r = {0.1f, 0.01f, 0.001f};
    return r;
}
SynapseModelExplanation synapse_ml_explain(uint64_t handle, const SynapseFeatureVector* fv) {
    (void)handle; (void)fv;
    SynapseModelExplanation r = {{0}, {0}, 0.9f, "v1", "ok"};
    return r;
}
const char* synapse_ml_last_error(void) { return 0; }
void synapse_ml_free_string(char* s) { (void)s; }
