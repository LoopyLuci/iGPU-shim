#pragma once
#include <string>
#include "telemetry_types.h"

namespace synapse {
bool write_session_report(const synapse::telemetry::SynapseSessionReport& report,
                         const std::string& path);
}
