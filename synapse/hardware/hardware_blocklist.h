// ============================================================================
// synapse/hardware/hardware_blocklist.h
// Project Synapse – Hardware Allowlist/Blocklist for Draw Telemetry
//
// At startup, queries the current GPU hardware and checks against known
// broken configurations. If the hardware is blocklisted, draw telemetry
// is disabled and the layer falls back to compute/bandwidth telemetry.
// ============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace synapse::hardware {

// GPU hardware identifier
struct GPUConfig {
    uint32_t vendor_id{0};      // PCI vendor ID (e.g., 0x8086 = Intel)
    uint32_t device_id{0};      // PCI device ID (e.g., 0x9B41 = UHD 630)
    uint32_t driver_version{0}; // Driver version as packed uint32 (major<<22 | minor<<12 | patch)
    std::string driver_string;  // Raw driver version string for logging
};

// Blocklist entry: hardware that is known to crash on draw telemetry
struct BlocklistEntry {
    GPUConfig config;
    const char* reason;
    const char* fallback;  // What to use instead
};

// Known broken configurations
// Driver version 27.20.100.9466 = 0x01B006E02 (packed)
// Intel vendor ID: 0x8086
// UHD 630 device ID: 0x9B41
static constexpr uint32_t kIntelVendorId = 0x8086;
static constexpr uint32_t kUHD630DeviceId = 0x9B41;
static constexpr uint32_t kDriver9466 = 0x01B006E02;  // 27.20.100.9466

static const std::vector<BlocklistEntry> kDrawTelemetryBlocklist = {
    {
        {kIntelVendorId, kUHD630DeviceId, kDriver9466, "27.20.100.9466"},
        "Intel UHD 630 driver 9466 crashes on any GPU work recording "
        "(vkCmdDraw, vkCmdDispatch, vkCmdCopyBuffer all crash at record time)",
        "compute/bandwidth telemetry"
    },
};

// Check if draw telemetry is allowed for the given hardware config
inline bool is_draw_telemetry_allowed(const GPUConfig& gpu) {
    for (const auto& entry : kDrawTelemetryBlocklist) {
        if (gpu.vendor_id == entry.config.vendor_id &&
            gpu.device_id == entry.config.device_id &&
            gpu.driver_version == entry.config.driver_version) {
            return false;
        }
    }
    return true;
}

// Pack a driver version string (e.g., "27.20.100.9466") into a uint32
// Format: major<<22 | minor<<12 | patch (matches DXGI packing)
inline uint32_t pack_driver_version(uint32_t major, uint32_t minor, uint32_t patch) {
    return (major << 22) | (minor << 12) | patch;
}

// Parse a dotted driver version string into packed uint32
// Handles formats: "A.B.C.D" (4-part) or "A.B.C" (3-part)
inline uint32_t parse_driver_version(const std::string& version_str) {
    uint32_t parts[4] = {0, 0, 0, 0};
    uint32_t idx = 0;
    uint32_t current = 0;

    for (char c : version_str) {
        if (c == '.') {
            if (idx < 4) parts[idx++] = current;
            current = 0;
        } else if (c >= '0' && c <= '9') {
            current = current * 10 + (c - '0');
        }
    }
    if (idx < 4) parts[idx] = current;

    return (parts[0] << 22) | (parts[1] << 12) | parts[2];
}

}  // namespace synapse::hardware
