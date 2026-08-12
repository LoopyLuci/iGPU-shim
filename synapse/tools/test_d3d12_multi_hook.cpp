/**
 * @file test_d3d12_multi_hook.cpp
 * @brief Validate simultaneous D3D12 multi-entry vtable hooking.
 *
 * Hooks DrawInstanced, DrawIndexedInstanced, Dispatch on the command list
 * vtable, and dynamically probes queue vtable indices to find ExecuteCommandLists.
 * Verifies each hook fires independently, then restores and validates cleanup.
 */

#if defined(_WIN32)
# include <windows.h>
# include <d3d12.h>
# include <dxgi1_6.h>
#endif

#include <cassert>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static bool g_draw_instanced_fired = false;
static bool g_draw_indexed_instanced_fired = false;
static bool g_dispatch_fired = false;
static bool g_execute_lists_fired = false;

extern "C" void __stdcall hook_draw_instanced(void*, UINT, UINT, UINT, UINT) {
    g_draw_instanced_fired = true;
}
extern "C" void __stdcall hook_draw_indexed_instanced(void*, UINT, UINT, UINT, int, UINT) {
    g_draw_indexed_instanced_fired = true;
}
extern "C" void __stdcall hook_dispatch(void*, UINT, UINT, UINT) {
    g_dispatch_fired = true;
}
extern "C" void __stdcall hook_execute_lists(void*, UINT, ID3D12CommandList* const*) {
    g_execute_lists_fired = true;
}

struct HookState {
    void** vtable;
    void* original;
    DWORD old_protect;
    size_t index;
};

static bool install_hook(HookState& hs, void** vtable, size_t idx, void* replacement) {
    hs.vtable = vtable;
    hs.index = idx;
    hs.original = vtable[idx];
    if (!VirtualProtect(&vtable[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &hs.old_protect))
        return false;
    vtable[idx] = replacement;
    return true;
}

static void restore_hook(HookState& hs) {
    if (!hs.vtable) return;
    hs.vtable[hs.index] = hs.original;
    DWORD ignored;
    VirtualProtect(&hs.vtable[hs.index], sizeof(void*), hs.old_protect, &ignored);
    hs.vtable = nullptr;
}

int main() {
    printf("=== D3D12 Multi-Entry Hook Test ===\n");

    ID3D12Device* device = nullptr;
    // -- device --
    {
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr) || !device) {
            printf("NOTE: D3D12 device creation failed with 0x%08lx; skipping.\n", hr);
            printf("Result: PASS\n");
            return 0;
        }
    }

    // -- command allocator --
    ID3D12CommandAllocator* allocator = nullptr;
    {
        HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(hr) || !allocator) { device->Release(); printf("FAIL: allocator\n"); return 1; }
    }

    // -- command list --
    ID3D12GraphicsCommandList* list = nullptr;
    {
        HRESULT hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
        if (FAILED(hr) || !list) { allocator->Release(); device->Release(); printf("FAIL: list\n"); return 1; }
    }

    // -- command queue --
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    {
        HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr) || !queue) { list->Release(); allocator->Release(); device->Release(); printf("FAIL: queue\n"); return 1; }
    }

    void** list_vtable = *reinterpret_cast<void***>(list);
    void** queue_vtable = *reinterpret_cast<void***>(queue);
    printf("  List vtable: %p, Queue vtable: %p\n", (void*)list_vtable, (void*)queue_vtable);

    // -- Step 1: install 3 command-list hooks (known good indices) --
    HookState cmd_hooks[3];
    memset(cmd_hooks, 0, sizeof(cmd_hooks));
    if (!install_hook(cmd_hooks[0], list_vtable, 12, (void*)hook_draw_instanced) ||
        !install_hook(cmd_hooks[1], list_vtable, 13, (void*)hook_draw_indexed_instanced) ||
        !install_hook(cmd_hooks[2], list_vtable, 14, (void*)hook_dispatch)) {
        for (int i = 0; i < 3; ++i) restore_hook(cmd_hooks[i]);
        list->Release(); allocator->Release(); queue->Release(); device->Release();
        printf("Result: FAIL (cmd hook install)\n");
        return 1;
    }
    printf("  3 cmd-list hooks installed (DrawInst=12, DrawIdx=13, Dispatch=14).\n");

    // -- Step 2: probe queue vtable indices for ExecuteCommandLists --
    // Candidate indices from SDK inheritance analysis:
    //   3  = vtable dump tool label (may be wrong)
    //   8  = if no ID3D12Pageable inheritance
    //   10 = if ID3D12Pageable adds no new slots
    //   13 = start of ID3D12CommandQueue methods (after Pageable's 5 slots)
    //   15 = ExecuteCommandLists (UpdateTileMappings=13, CopyTileMappings=14, Exec=15)
    constexpr size_t kProbeIndices[] = {3, 8, 10, 13, 15};
    size_t found_idx = (size_t)-1;

    printf("  Probing queue vtable indices for ExecuteCommandLists:\n");
    for (int p = 0; p < 5; ++p) {
        size_t idx = kProbeIndices[p];
        void* orig = queue_vtable[idx];
        printf("    [%zu] orig=%p ... ", idx, orig);

        // Install probe hook
        DWORD old_protect = 0;
        if (!VirtualProtect(&queue_vtable[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
            printf("VirtualProtect FAILED\n");
            continue;
        }
        queue_vtable[idx] = (void*)hook_execute_lists;

        // Call ExecuteCommandLists
        g_execute_lists_fired = false;
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);

        // Restore
        queue_vtable[idx] = orig;
        DWORD ignored;
        VirtualProtect(&queue_vtable[idx], sizeof(void*), old_protect, &ignored);

        printf("hook fired = %s\n", g_execute_lists_fired ? "YES" : "NO");
        if (g_execute_lists_fired) {
            found_idx = idx;
            break;
        }
    }

    if (found_idx == (size_t)-1) {
        for (int i = 0; i < 3; ++i) restore_hook(cmd_hooks[i]);
        list->Release(); allocator->Release(); queue->Release(); device->Release();
        printf("Result: FAIL (ExecuteCommandLists not found at indices %zu,%zu,%zu,%zu,%zu)\n",
               kProbeIndices[0], kProbeIndices[1], kProbeIndices[2],
               kProbeIndices[3], kProbeIndices[4]);
        return 1;
    }
    printf("  ExecuteCommandLists confirmed at queue vtable index %zu.\n", found_idx);

    // -- Step 3: record draw/dispatch calls and verify cmd hooks --
    list->DrawInstanced(3, 1, 0, 0);
    list->DrawIndexedInstanced(3, 1, 0, 0, 0);
    list->Dispatch(1, 1, 1);

    printf("  DrawInstanced fired:        %s\n", g_draw_instanced_fired ? "YES" : "NO");
    printf("  DrawIndexedInstanced fired: %s\n", g_draw_indexed_instanced_fired ? "YES" : "NO");
    printf("  Dispatch fired:             %s\n", g_dispatch_fired ? "YES" : "NO");

    HRESULT hr = list->Close();
    printf("  Command list Close: 0x%08lx\n", hr);

    // -- Step 4: install hook at confirmed index and fire ExecuteCommandLists --
    HookState queue_hook;
    memset(&queue_hook, 0, sizeof(queue_hook));
    if (!install_hook(queue_hook, queue_vtable, found_idx, (void*)hook_execute_lists)) {
        for (int i = 0; i < 3; ++i) restore_hook(cmd_hooks[i]);
        list->Release(); allocator->Release(); queue->Release(); device->Release();
        printf("Result: FAIL (queue hook install at found index failed)\n");
        return 1;
    }
    printf("  Queue hook installed at index %zu for final verification.\n", found_idx);

    g_execute_lists_fired = false;
    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    printf("  ExecuteCommandLists fired (final): %s\n", g_execute_lists_fired ? "YES" : "NO");

    // -- Step 5: restore all hooks --
    restore_hook(queue_hook);
    for (int i = 0; i < 3; ++i) restore_hook(cmd_hooks[i]);

    // -- Step 6: verify list still usable --
    hr = list->Reset(allocator, nullptr);
    printf("  List Reset after restore: 0x%08lx\n", hr);

    list->Release();
    allocator->Release();
    queue->Release();
    device->Release();

    if (!g_draw_instanced_fired || !g_draw_indexed_instanced_fired || !g_dispatch_fired || !g_execute_lists_fired) {
        printf("Result: FAIL (not all hooks fired)\n");
        return 1;
    }
    printf("Result: PASS\n");
    return 0;
}
#else
int main() {
    printf("NOTE: D3D12 multi-hook test skipped on non-Windows platform.\n");
    printf("Result: PASS\n");
    return 0;
}
#endif
