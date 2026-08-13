#include "json_report_writer.h"
#include <fstream>
#include <sstream>
#include <iomanip>

namespace synapse {

namespace {
std::string escape_json(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (char c : s) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(c));
                } else {
                    out << c;
                }
        }
    }
    out << '"';
    return out.str();
}

std::string to_json(double v) {
    std::ostringstream out;
    out << std::setprecision(15) << v;
    return out.str();
}

std::string to_json(uint64_t v) {
    return std::to_string(v);
}

std::string to_json(uint32_t v) {
    return std::to_string(v);
}

std::string to_json(const std::string& v) {
    return escape_json(v);
}

std::string to_json(const std::vector<synapse::telemetry::HorizonStats>& v) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out << ",";
        out << "{";
        out << "\"window\":" << to_json(v[i].window) << ",";
        out << "\"stalls_avoided\":" << to_json(v[i].stalls_avoided) << ",";
        out << "\"false_positives\":" << to_json(v[i].false_positives) << ",";
        out << "\"energy_efficiency\":" << to_json(v[i].energy_efficiency());
        out << "}";
    }
    out << "]";
    return out.str();
}

std::string to_json(const synapse::telemetry::PredictionStats& v) {
    std::ostringstream out;
    out << "{";
    out << "\"total_predictions\":" << to_json(v.total_predictions) << ",";
    out << "\"accurate_predictions\":" << to_json(v.accurate_predictions) << ",";
    out << "\"wasted_predictions\":" << to_json(v.wasted_predictions) << ",";
    out << "\"accuracy_rate\":" << to_json(v.accuracy_rate()) << ",";
    out << "\"waste_rate\":" << to_json(v.waste_rate());
    out << "}";
    return out.str();
}

std::string to_json(const synapse::telemetry::CacheMetrics& v) {
    std::ostringstream out;
    out << "{";
    out << "\"hits\":" << to_json(v.hits) << ",";
    out << "\"misses\":" << to_json(v.misses) << ",";
    out << "\"sync_stalls\":" << to_json(v.sync_stalls) << ",";
    out << "\"current_usage_bytes\":" << to_json(v.current_usage_bytes) << ",";
    out << "\"hit_rate\":" << to_json(v.hit_rate());
    out << "}";
    return out.str();
}

std::string to_json(const synapse::telemetry::MLTrainingStats& v) {
    std::ostringstream out;
    out << "{";
    out << "\"total_updates\":" << to_json(v.total_updates) << ",";
    out << "\"cumulative_reward\":" << to_json(v.cumulative_reward) << ",";
    out << "\"selection_counts\":[" << to_json(v.selection_counts[0]) << "," << to_json(v.selection_counts[1]) << "," << to_json(v.selection_counts[2]) << "],";
    out << "\"current_epsilon\":" << to_json(v.current_epsilon) << ",";
    out << "\"current_alpha\":" << to_json(v.current_alpha) << ",";
    out << "\"training_status\":" << to_json(v.training_status);
    out << "}";
    return out.str();
}

std::string to_json(const synapse::telemetry::DVFSStats& v) {
    std::ostringstream out;
    out << "{";
    out << "\"total_transitions\":" << to_json(v.total_transitions) << ",";
    out << "\"emergency_overrides\":" << to_json(v.emergency_overrides) << ",";
    out << "\"hysteresis_drops\":" << to_json(v.hysteresis_drops) << ",";
    out << "\"total_switch_energy_nj\":" << to_json(v.total_switch_energy_nj);
    out << "}";
    return out.str();
}

std::string to_json(const synapse::telemetry::JITStats& v) {
    std::ostringstream out;
    out << "{";
    out << "\"cold_cache_fallbacks\":" << to_json(v.cold_cache_fallbacks) << ",";
    out << "\"cache_hits\":" << to_json(v.cache_hits) << ",";
    out << "\"worst_fallback_ms\":" << to_json(v.worst_fallback_ms) << ",";
    out << "\"total_fallback_ms\":" << to_json(v.total_fallback_ms);
    out << "}";
    return out.str();
}

std::string to_json(const synapse::telemetry::HAIStats& v) {
    std::ostringstream out;
    out << "{";
    out << "\"full_draws_emitted\":" << to_json(v.full_draws_emitted) << ",";
    out << "\"delta_draws_emitted\":" << to_json(v.delta_draws_emitted) << ",";
    out << "\"raw_bytes_equivalent\":" << to_json(v.raw_bytes_equivalent) << ",";
    out << "\"actual_bytes_emitted\":" << to_json(v.actual_bytes_emitted) << ",";
    out << "\"compression_ratio\":" << to_json(v.compression_ratio());
    out << "}";
    return out.str();
}

std::string to_json(const synapse::telemetry::ThermalStats& v) {
    std::ostringstream out;
    out << "{";
    out << "\"thermal_mitigation_events\":" << to_json(v.thermal_mitigation_events) << ",";
    out << "\"stability_overrides\":" << to_json(v.stability_overrides) << ",";
    out << "\"proactive_boosts\":" << to_json(v.proactive_boosts);
    out << "}";
    return out.str();
}

std::string to_json(const synapse::telemetry::PowerReport& v) {
    std::ostringstream out;
    out << "{";
    out << "\"joules_saved\":" << to_json(v.joules_saved) << ",";
    out << "\"avg_milliwatts_saved_at_60fps\":" << to_json(v.avg_milliwatts_saved_at_60fps) << ",";
    out << "\"battery_extension_factor\":" << to_json(v.battery_extension_factor);
    out << "}";
    return out.str();
}

std::string to_json(const synapse::telemetry::BackendRoutingSummary& v) {
    std::ostringstream out;
    out << "{";
    out << "\"jit_dispatches\":" << to_json(v.jit_dispatches) << ",";
    out << "\"hai_dispatches\":" << to_json(v.hai_dispatches) << ",";
    out << "\"oracle_dispatches\":" << to_json(v.oracle_dispatches) << ",";
    out << "\"total_draw_calls\":" << to_json(v.total_draw_calls);
    out << "}";
    return out.str();
}

std::string to_json(const synapse::telemetry::HorizonStats& v) {
    std::ostringstream out;
    out << "{";
    out << "\"window\":" << to_json(v.window) << ",";
    out << "\"stalls_avoided\":" << to_json(v.stalls_avoided) << ",";
    out << "\"false_positives\":" << to_json(v.false_positives) << ",";
    out << "\"energy_efficiency\":" << to_json(v.energy_efficiency());
    out << "}";
    return out.str();
}

std::string to_json(const synapse::telemetry::SynapseSessionReport& r) {
    std::ostringstream out;
    out << "{";
    out << "\"schema_version\":\"v2.0.0\",";
    out << "\"heuristic\":" << to_json(r.heuristic) << ",";
    out << "\"temporal_window_frames\":" << to_json(r.temporal_window_frames) << ",";
    out << "\"its_predictor\":" << to_json(r.its_predictor) << ",";
    out << "\"its_cache\":" << to_json(r.its_cache) << ",";
    out << "\"best_horizon\":" << to_json(r.best_horizon) << ",";
    out << "\"horizon_windows\":" << to_json(r.horizon_windows) << ",";
    out << "\"total_frames_analyzed\":" << to_json(r.total_frames_analyzed) << ",";
    out << "\"ml_model\":" << to_json(r.ml_model) << ",";
    out << "\"dvfs\":" << to_json(r.dvfs) << ",";
    out << "\"jit\":" << to_json(r.jit) << ",";
    out << "\"hai\":" << to_json(r.hai) << ",";
    out << "\"thermal\":" << to_json(r.thermal) << ",";
    out << "\"power\":" << to_json(r.power) << ",";
    out << "\"backend_routing\":" << to_json(r.backend_routing);
    out << "}";
    return out.str();
}
} // namespace

bool write_session_report(const synapse::telemetry::SynapseSessionReport& report,
                          const std::string& path) {
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs << to_json(report);
    return true;
}

} // namespace synapse
