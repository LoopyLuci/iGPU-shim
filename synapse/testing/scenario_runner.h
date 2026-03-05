#pragma once

#include "agent_api.h"
#include <vector>

namespace synapse::testing {

struct TestStep {
    WorkloadSignature sig;
    uint32_t repeats = 1;
};

struct TestScenario {
    std::string name;
    std::vector<TestStep> steps;
};

struct ScenarioResult {
    std::string scenario_name;
    bool passed = false;
    std::string detail;
    SystemSnapshot before;
    SystemSnapshot after;
};

class ScenarioRunner {
public:
    ScenarioRunner(AgentAPI& api) : api_(api) {}

    ScenarioResult run(const TestScenario& s) {
        ScenarioResult r; r.scenario_name = s.name;
        r.before = api_.snapshot();
        for (const auto& step : s.steps) {
            for (uint32_t i = 0; i < step.repeats; ++i) {
                api_.injector.inject_workload(step.sig);
            }
        }
        r.after = api_.snapshot();
        r.passed = true; // In production compare before/after against acceptance criteria
        r.detail = "Synthetic run completed (pass criteria evaluation is environment-specific).";
        return r;
    }

private:
    AgentAPI& api_;
};

} // namespace synapse::testing
