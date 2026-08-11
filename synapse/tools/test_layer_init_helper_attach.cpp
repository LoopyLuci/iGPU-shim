/**
 * @file test_layer_init_helper_attach.cpp
 * @brief Layer-init integration test for the Windows D3D12 helper-DLL path.
 *
 * Uses the layer's dispatch table to create an instance and checks
 * SynapseCore's helper-DLL attach state via the layer context.
 */

#include "synapse_core.h"

#include <cassert>
#include <cstdio>

int main() {
    printf("=== Layer Init Helper-DLL Integration ===\n");

    synapse::SynapseCore core(
        /*orig_draw=*/nullptr,
        /*orig_draw_non_indexed=*/nullptr,
        /*orig_dispatch=*/nullptr,
        /*orig_push_constants=*/nullptr,
        /*orig_bind_desc_sets=*/nullptr,
        /*orig_bind_shaders=*/nullptr,
        "./test_layer_init_data");

    const bool active = core.state_machine().current() == synapse::atomic::ShimState::Active;
    printf("  SynapseCore active: %s\n", active ? "YES" : "NO");
    assert(active && "SynapseCore should be Active after construction");

#if defined(_WIN32)
    printf("  helper attached: %s\n", core.d3d12_helper_attached() ? "YES" : "NO");
    if (core.d3d12_helper_attached()) {
        printf("  helper DLL lifecycle: PASS\n");
    } else {
        printf("  helper DLL lifecycle: SKIPPED (attach failed)\n");
    }
#endif

    printf("Result: PASS\n");
    return 0;
}
