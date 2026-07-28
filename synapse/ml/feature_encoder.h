#pragma once

#include "synapse_umd.h"
#include "../telemetry_types.h"
#include <array>
#include <cmath>
#include <algorithm>

namespace synapse::ml {

// Encodes WorkloadSignature into a fixed-length feature vector of length 8.
class FeatureEncoder {
public:
    using Features = std::array<float,8>;

    // If `report` is provided, the encoder will incorporate live telemetry
    // (cache hit rate, dvfs/thermal headroom, predicted bandwidth).
    Features encode(const WorkloadSignature& sig, const synapse::telemetry::SynapseSessionReport* report = nullptr) const {
        Features f{};
        // 0: shader_complexity_normalized (0..1)
        f[0] = std::clamp(sig.shader_instruction_estimate / 2000.0f, 0.0f, 1.0f);
        // 1: vertex_count_log (log normalize)
        f[1] = std::clamp(std::log10(static_cast<float>(sig.vertex_count) + 1.0f) / 4.0f, 0.0f, 1.0f);
        // 2: draw_call_rate (placeholder, callers may overwrite)
        f[2] = std::clamp(sig.draw_call_count / 100.0f, 0.0f, 1.0f);
        // 3: cache_hit_rate (use ITS cache hit-rate if available)
        if (report) {
            f[3] = std::clamp(report->its_cache.hit_rate(), 0.0f, 1.0f);
        } else {
            f[3] = 0.5f;
        }

        // 4: dvfs_headroom — linear expression: lower on frequent transitions,
        //    further penalised by emergency overrides (T2-5 precision fix).
        if (report) {
            f[4] = std::clamp(
                1.0f - static_cast<float>(report->dvfs.total_transitions) / 50.0f
                     + static_cast<float>(report->dvfs.emergency_overrides) * -0.2f,
                0.0f, 1.0f);
        } else {
            f[4] = 0.5f;
        }

        // 5: thermal_headroom — map mitigation events into a conservative headroom estimate.
        if (report) {
            float mitigation = static_cast<float>(report->thermal.thermal_mitigation_events);
            float proactive = static_cast<float>(report->thermal.proactive_boosts);
            float penalty = std::min(1.0f, mitigation * 0.1f);
            float boost = std::min(0.5f, proactive * 0.02f);
            f[5] = std::clamp(1.0f - penalty + boost, 0.0f, 1.0f);
        } else {
            f[5] = 0.5f;
        }

        // 6: predicted_bandwidth_normalized — use ITS predictor accuracy as a proxy
        if (report) {
            f[6] = std::clamp(static_cast<float>(report->its_predictor.accuracy_rate()), 0.0f, 1.0f);
        } else {
            f[6] = 0.5f;
        }
        // 7: is_compute_dispatch
        f[7] = sig.is_compute_dispatch ? 1.0f : 0.0f;
        return f;
    }
};

} // namespace synapse::ml
