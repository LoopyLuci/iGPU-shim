#include <iostream>
#include <string>
#include "../testing/agent_api.h"
#include "../testing/scenario_runner.h"
#include "../ml/ml_sub_api.h"

int main(int argc, char** argv) {
    using namespace synapse::testing;
    using namespace synapse::ml;
    AgentAPI api;
    ScenarioRunner runner(api);
    MLSubAPI ml;

    if (argc < 2) {
        std::cout << "synapse_cli: snapshot | inject | run-scenario | coverage | train | explain\n";
        return 0;
    }
    std::string cmd = argv[1];
    if (cmd == "snapshot") {
        auto s = api.snapshot();
        std::cout << "Snapshot: (report heuristic) " << s.report.heuristic << "\n";
    } else if (cmd == "coverage") {
        std::cout << "Coverage: experimental (use test harness for full run)\n";
    } else if (cmd == "train") {
        if (argc >= 3 && std::string(argv[2]) == "checkpoint") {
            const std::string path = (argc>=4) ? argv[3] : "bandit.chk";
            ml.checkpoint(path);
            std::cout << "ML checkpoint saved to " << path << "\n";
        } else if (argc >= 3 && std::string(argv[2]) == "restore") {
            const std::string path = (argc>=4) ? argv[3] : "bandit.chk";
            ml.restore(path);
            std::cout << "ML restored from " << path << "\n";
        } else {
            std::cout << "train subcommands: checkpoint <path> | restore <path>\n";
        }
    } else if (cmd == "explain") {
        int action = (argc>=3) ? std::atoi(argv[2]) : 0;
        auto w = ml.explain_action(action);
        std::cout << "Weights for action " << action << ": ";
        for (float v : w) std::cout << v << " ";
        std::cout << "\n";
    } else if (cmd == "run-scenario") {
        // Minimal example: run a trivial scenario
        synapse::testing::TestScenario s; s.name = "cli-mini";
        synapse::WorkloadSignature sig{}; sig.vertex_count = 1000; sig.draw_call_count = 1;
        synapse::testing::TestStep step{sig, 10};
        s.steps.push_back(step);
        auto res = runner.run(s);
        std::cout << "Scenario " << res.scenario_name << " completed. passed=" << res.passed << "\n";
    } else {
        std::cout << "Unknown command. Available: snapshot inject run-scenario coverage train explain\n";
    }
    return 0;
}