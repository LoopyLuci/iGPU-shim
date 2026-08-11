/**
 * @file test_synapse_core_helper_attach.cpp
 * @brief Validates that SynapseCore construction runs the D3D12 helper-DLL
 * attach path without crashing, and that the resulting object is usable.
 */

#include "synapse_core.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    printf("=== SynapseCore helper-DLL attach test ===\n");

    // Provide minimal stub function pointers; on Windows the constructor
    // will attempt to attach SynapseD3D12Helper.dll as part of startup.
    synapse::SynapseCore core(
        /*orig_draw=*/nullptr,
        /*orig_draw_non_indexed=*/nullptr,
        /*orig_dispatch=*/nullptr,
        /*orig_push_constants=*/nullptr,
        /*orig_bind_desc_sets=*/nullptr,
        /*orig_bind_shaders=*/nullptr,
        "./test_synapse_core_data");

    // Verify core transitioned out of Uninitialized.
    const bool active = core.state_machine().current() == synapse::atomic::ShimState::Active;
    printf("  SynapseCore active: %s\n", active ? "YES" : "NO");
    assert(active && "SynapseCore should be Active after construction");

#if defined(_WIN32)
    printf("  helper attached: %s\n", core.d3d12_helper_attached() ? "YES" : "NO");
#endif

    printf("Result: PASS\n");
    return 0;
}
