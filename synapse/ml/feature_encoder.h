#pragma once

#include "synapse_umd.h"
#include <array>
#include <cmath>

namespace synapse::ml {

// Encodes WorkloadSignature into a fixed-length feature vector of length 8.
class FeatureEncoder {
public:
    using Features = std::array<float,8>;

    Features encode(const WorkloadSignature& sig) const {
        Features f{};
        // 0: shader_complexity_normalized (0..1)
        f[0] = std::clamp(sig.shader_instruction_estimate / 2000.0f, 0.0f, 1.0f);
        // 1: vertex_count_log (log normalize)
        f[1] = std::clamp(std::log10(static_cast<float>(sig.vertex_count) + 1.0f) / 4.0f, 0.0f, 1.0f);
        // 2: draw_call_rate (placeholder, callers may overwrite)
        f[2] = std::clamp(sig.draw_call_count / 100.0f, 0.0f, 1.0f);
        // 3: cache_hit_rate (unknown here) -> 0.5 default
        f[3] = 0.5f;
        // 4: dvfs_headroom (unknown here) -> 0.5 default
        f[4] = 0.5f;
        // 5: thermal_headroom (unknown here) -> 0.5 default
        f[5] = 0.5f;
        // 6: predicted_bandwidth_normalized (placeholder)
        f[6] = 0.5f;
        // 7: is_compute_dispatch
        f[7] = sig.is_compute_dispatch ? 1.0f : 0.0f;
        return f;
    }
};

} // namespace synapse::ml
