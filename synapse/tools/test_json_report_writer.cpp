#include <cassert>
#include <fstream>
#include "synapse_core.h"
#include "json_report_writer.h"

int main() {
    synapse::telemetry::SynapseSessionReport r{};
    r.heuristic = "temporal_locality_v1";
    r.temporal_window_frames = 3;
    r.its_predictor.total_predictions = 10;
    r.its_predictor.accurate_predictions = 8;
    r.its_cache.hits = 7;
    r.its_cache.misses = 3;
    r.ml_model.total_updates = 4;
    r.ml_model.cumulative_reward = 1.2;
    r.ml_model.training_status = "active";
    r.backend_routing.jit_dispatches = 1;
    r.backend_routing.hai_dispatches = 2;
    r.backend_routing.oracle_dispatches = 3;
    r.backend_routing.total_draw_calls = 6;

    const std::string path = "report.json";
    bool ok = synapse::write_session_report(r, path);
    assert(ok);

    std::ifstream ifs(path);
    assert(ifs.is_open());
    std::string body((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    assert(body.find("\"schema_version\":\"v2.0.0\"") != std::string::npos);
    assert(body.find("\"total_predictions\":10") != std::string::npos);
    assert(body.find("\"training_status\":\"active\"") != std::string::npos);
    return 0;
}
