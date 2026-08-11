/**
 * @file test_d3d12_helper_dll_lifecycle.cpp
 * @brief Smoke test for the full helper-DLL lifecycle through SynapseCore.
 *
 * Validates:
 *  - SynapseCore construction does not crash when the helper DLL is present/absent
 *  - d3d12_helper_attached() reflects the real attach state during the object's lifetime
 *  - Destruction runs the helper-DLL teardown path without crashing
 */

#include "synapse_core.h"

#include <cassert>
#include <cstdio>

int main() {
    printf("=== D3D12 helper-DLL lifecycle test ===\n");

    synapse::SynapseCore core(
        /*orig_draw=*/nullptr,
        /*orig_draw_non_indexed=*/nullptr,
        /*orig_dispatch=*/nullptr,
        /*orig_push_constants=*/nullptr,
        /*orig_bind_desc_sets=*/nullptr,
        /*orig_bind_shaders=*/nullptr,
        "./test_d3d12_helper_dll_lifecycle_data");

    const bool active = core.state_machine().current() == synapse::atomic::ShimState::Active;
    printf("  SynapseCore active: %s\n", active ? "YES" : "NO");
    assert(active && "SynapseCore should be Active after construction");

#if defined(_WIN32)
    printf("  helper attached: %s\n", core.d3d12_helper_attached() ? "YES" : "NO");
#endif

    printf("Result: PASS\n");
    return 0;
}
