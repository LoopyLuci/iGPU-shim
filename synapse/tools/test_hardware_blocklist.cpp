/**
 * @file test_hardware_blocklist.cpp
 * @brief Unit tests for hardware allowlist/blocklist system.
 */

#include "../hardware/hardware_blocklist.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace synapse::hardware;

int main() {
    // ── Test 1: Pack driver version ────────────────────────────────────
    std::cout << "Test 1: Pack driver version... ";
    {
        uint32_t packed = pack_driver_version(27, 20, 100);
        assert(packed == ((27 << 22) | (20 << 12) | 100)); (void)packed;
        std::cout << "PASS\n";
    }

    // ── Test 2: Parse driver version string ────────────────────────────
    std::cout << "Test 2: Parse driver version string... ";
    {
        uint32_t parsed = parse_driver_version("27.20.100.9466");
        assert(parsed == pack_driver_version(27, 20, 100));
        std::cout << "PASS\n";
    }

    // ── Test 3: Blocklisted hardware returns false ─────────────────────
    std::cout << "Test 3: Blocklisted hardware returns false... ";
    {
        GPUConfig gpu{};
        gpu.vendor_id = kIntelVendorId;
        gpu.device_id = kUHD630DeviceId;
        gpu.driver_version = kDriver9466;
        gpu.driver_string = "27.20.100.9466";

        assert(!is_draw_telemetry_allowed(gpu));
        std::cout << "PASS\n";
    }

    // ── Test 4: Unknown hardware returns true ──────────────────────────
    std::cout << "Test 4: Unknown hardware returns true... ";
    {
        GPUConfig gpu{};
        gpu.vendor_id = 0x10DE;  // NVIDIA
        gpu.device_id = 0x2684;  // RTX 4090
        gpu.driver_version = pack_driver_version(535, 0, 0);
        gpu.driver_string = "535.0.0";

        assert(is_draw_telemetry_allowed(gpu));
        std::cout << "PASS\n";
    }

    // ── Test 5: Same vendor/device, different driver → allowed ─────────
    std::cout << "Test 5: Same vendor/device, different driver → allowed... ";
    {
        GPUConfig gpu{};
        gpu.vendor_id = kIntelVendorId;
        gpu.device_id = kUHD630DeviceId;
        gpu.driver_version = pack_driver_version(30, 0, 0);  // Newer driver
        gpu.driver_string = "30.0.0";

        assert(is_draw_telemetry_allowed(gpu));
        std::cout << "PASS\n";
    }

    // ── Test 6: Same vendor, different device → allowed ────────────────
    std::cout << "Test 6: Same vendor, different device → allowed... ";
    {
        GPUConfig gpu{};
        gpu.vendor_id = kIntelVendorId;
        gpu.device_id = 0x9BC8;  // UHD 770
        gpu.driver_version = kDriver9466;
        gpu.driver_string = "27.20.100.9466";

        assert(is_draw_telemetry_allowed(gpu));
        std::cout << "PASS\n";
    }

    // ── Test 7: Blocklist size ─────────────────────────────────────────
    std::cout << "Test 7: Blocklist size... ";
    {
        assert(kDrawTelemetryBlocklist.size() >= 1);
        std::cout << "PASS (size=" << kDrawTelemetryBlocklist.size() << ")\n";
    }

    // ── Test 8: Blocklist entry has reason and fallback ────────────────
    std::cout << "Test 8: Blocklist entry has reason and fallback... ";
    {
        const auto& entry = kDrawTelemetryBlocklist[0];
        assert(entry.reason != nullptr);
        assert(entry.fallback != nullptr);
        assert(strlen(entry.reason) > 0);
        assert(strlen(entry.fallback) > 0);
        std::cout << "PASS\n";
    }

    // ── Test 9: Null GPU config (defaults) → allowed ──────────────────
    std::cout << "Test 9: Null GPU config (defaults) → allowed... ";
    {
        GPUConfig gpu{};  // All zeros
        assert(is_draw_telemetry_allowed(gpu));
        std::cout << "PASS\n";
    }

    // ── Test 10: Multiple blocklist entries ────────────────────────────
    std::cout << "Test 10: Multiple blocklist entries... ";
    {
        // Verify all entries are checked
        for (const auto& entry : kDrawTelemetryBlocklist) {
            assert(!is_draw_telemetry_allowed(entry.config));
        }
        std::cout << "PASS\n";
    }

    std::cout << "\nAll 10 tests passed.\n";
    std::cout << "Result: PASS\n";
    return 0;
}
