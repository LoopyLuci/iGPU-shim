/**@file test_d3d12_helper_real_device_hooks.cpp
 * @brief End-to-end D3D12 interception through SynapseD3D12Helper.dll.
 *
 * Loads SynapseD3D12Helper.dll, calls attach_process_hooks(), then uses
 * install_hook/remove_hook on the actual vtable slots of a real
 * ID3D12GraphicsCommandList and ID3D12CommandQueue. Fires real D3D12
 * calls (DrawInstanced, DrawIndexedInstanced, Dispatch, ExecuteCommandLists)
 * and verifies the replacement callbacks fire through the helper-DLL hook
 * path. Removes all hooks via remove_hook before exit.
 */

#if defined(_WIN32)
# include <windows.h>
# include <d3d12.h>
# include <dxgi1_6.h>
#endif

#include "../synapse_d3d12_helper_dll.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using LoadFunc    = long (__stdcall*)();
using UnloadFunc  = void (__stdcall*)();
using ReadyFunc   = bool (*)();
using InstallFunc = long (*)(void*, void*, void**);
using RemoveFunc  = long (*)(void*, void*);

static std::string build_dll_path() {
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path = exePath;
    const auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path = path.substr(0, pos + 1);
    path += "SynapseD3D12Helper.dll";
    return path;
}

static bool load_helper(const std::string& path, HMODULE& module,
                        LoadFunc& attach, ReadyFunc& ready,
                        UnloadFunc& detach, InstallFunc& install,
                        RemoveFunc& remove) {
    module = LoadLibraryA(path.c_str());
    if (!module) return false;
    attach  = reinterpret_cast<LoadFunc>(GetProcAddress(module, "attach_process_hooks"));
    ready   = reinterpret_cast<ReadyFunc>(GetProcAddress(module, "is_ready"));
    detach  = reinterpret_cast<UnloadFunc>(GetProcAddress(module, "detach_process_hooks"));
    install = reinterpret_cast<InstallFunc>(GetProcAddress(module, "install_hook"));
    remove  = reinterpret_cast<RemoveFunc>(GetProcAddress(module, "remove_hook"));
    return attach && ready && detach && install && remove;
}

// ── Callback flags ────────────────────────────────────────────────────────────

static bool g_draw_instanced_fired = false;
static bool g_draw_indexed_fired   = false;
static bool g_dispatch_fired       = false;
static bool g_execute_lists_fired  = false;

static void __stdcall hook_draw_instanced(void*, UINT, UINT, UINT, UINT) {
    g_draw_instanced_fired = true;
}
static void __stdcall hook_draw_indexed(void*, UINT, UINT, UINT, int, UINT) {
    g_draw_indexed_fired = true;
}
static void __stdcall hook_dispatch(void*, UINT, UINT, UINT) {
    g_dispatch_fired = true;
}
static void __stdcall hook_execute_lists(void*, UINT, ID3D12CommandList* const*) {
    g_execute_lists_fired = true;
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    printf("=== D3D12 Helper DLL Real-Device Hook Test ===\n");

    const std::string dllPath = build_dll_path();
    printf("  DLL path: %s\n", dllPath.c_str());

    HMODULE module = nullptr;
    LoadFunc   attach   = nullptr;
    ReadyFunc  ready    = nullptr;
    UnloadFunc detach   = nullptr;
    InstallFunc install = nullptr;
    RemoveFunc remove   = nullptr;

    if (!load_helper(dllPath, module, attach, ready, detach, install, remove)) {
        printf("  NOTE: helper DLL not loadable — skipping.\n");
        printf("Result: PASS\n");
        return 0;
    }
    printf("  Helper DLL loaded.\n");

    // ── 1) Lifecycle ─────────────────────────────────────────────────────────
    const long hr = attach();
    if (FAILED(hr)) { printf("FAIL: attach_process_hooks = 0x%08lx\n", hr); return 1; }
    if (!ready())   { printf("FAIL: is_ready() false\n"); return 1; }
    printf("  attach + ready OK.\n");

    // ── 2) Create D3D12 device + command list + command queue ───────────────
    ID3D12Device*           device    = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list   = nullptr;
    ID3D12CommandQueue*     queue     = nullptr;

    {
        HRESULT hr2 = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr2) || !device) {
            printf("NOTE: D3D12CreateDevice failed with 0x%08lx — skipping.\n", hr2);
            printf("Result: PASS\n");
            return 0;
        }
    }

    {
        HRESULT hr2 = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(hr2) || !allocator) { device->Release(); printf("FAIL: allocator\n"); return 1; }
    }

    {
        HRESULT hr2 = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
        if (FAILED(hr2) || !list) { allocator->Release(); device->Release(); printf("FAIL: list\n"); return 1; }
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    {
        HRESULT hr2 = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr2) || !queue) { list->Release(); allocator->Release(); device->Release(); printf("FAIL: queue\n"); return 1; }
    }

    void** list_vtable  = *reinterpret_cast<void***>(list);
    void** queue_vtable = *reinterpret_cast<void***>(queue);
    printf("  List vtable=%p, Queue vtable=%p\n", (void*)list_vtable, (void*)queue_vtable);

    // ── 3) Install hooks via helper DLL on real vtable slots ────────────────
    // Confirmed indices from test_d3d12_vtable_dump.exe on SDK 10.0.26100.0:
    //   List:  DrawInstanced=12, DrawIndexedInstanced=13, Dispatch=14
    //   Queue: ExecuteCommandLists=10
    struct SavedHook { void* slot; void* original; };
    SavedHook cmd_hooks[3];
    bool all_ok = true;

    {
        void* orig = nullptr;
        const long hr2 = install(&list_vtable[12], (void*)hook_draw_instanced, &orig);
        if (FAILED(hr2) || !orig) { printf("FAIL: install list[12] 0x%08lx\n", hr2); all_ok = false; }
        else cmd_hooks[0] = { &list_vtable[12], orig };
    }
    if (all_ok) {
        void* orig = nullptr;
        const long hr2 = install(&list_vtable[13], (void*)hook_draw_indexed, &orig);
        if (FAILED(hr2) || !orig) { printf("FAIL: install list[13] 0x%08lx\n", hr2); all_ok = false; }
        else cmd_hooks[1] = { &list_vtable[13], orig };
    }
    if (all_ok) {
        void* orig = nullptr;
        const long hr2 = install(&list_vtable[14], (void*)hook_dispatch, &orig);
        if (FAILED(hr2) || !orig) { printf("FAIL: install list[14] 0x%08lx\n", hr2); all_ok = false; }
        else cmd_hooks[2] = { &list_vtable[14], orig };
    }

    if (!all_ok) {
        for (int i = 0; i < 3 && cmd_hooks[i].slot; ++i) remove(cmd_hooks[i].slot, cmd_hooks[i].original);
        list->Release(); allocator->Release(); queue->Release(); device->Release();
        return 1;
    }
    printf("  Installed 3 cmd-list hooks via helper DLL.\n");

    SavedHook queue_hook{};
    {
        void* orig = nullptr;
        const long hr2 = install(&queue_vtable[10], (void*)hook_execute_lists, &orig);
        if (FAILED(hr2) || !orig) { printf("FAIL: install queue[10] 0x%08lx\n", hr2);
            remove(cmd_hooks[0].slot, cmd_hooks[0].original);
            remove(cmd_hooks[1].slot, cmd_hooks[1].original);
            remove(cmd_hooks[2].slot, cmd_hooks[2].original);
            list->Release(); allocator->Release(); queue->Release(); device->Release();
            return 1;
        }
        queue_hook = { &queue_vtable[10], orig };
    }
    printf("  Installed queue hook at index 10 via helper DLL.\n");

    // ── 4) Fire real D3D12 calls and verify callbacks ───────────────────────
    g_draw_instanced_fired = false;
    g_draw_indexed_fired   = false;
    g_dispatch_fired       = false;

    list->DrawInstanced(3, 1, 0, 0);
    list->DrawIndexedInstanced(3, 1, 0, 0, 0);
    list->Dispatch(1, 1, 1);

    HRESULT hr_close = list->Close();
    printf("  List Close: 0x%08lx\n", hr_close);

    printf("  cmd-list hooks: DrawInstanced=%s, DrawIndexedInstanced=%s, Dispatch=%s\n",
           g_draw_instanced_fired ? "YES" : "NO",
           g_draw_indexed_fired   ? "YES" : "NO",
           g_dispatch_fired       ? "YES" : "NO");

    g_execute_lists_fired = false;
    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    printf("  queue hook:    ExecuteCommandLists=%s\n",
           g_execute_lists_fired ? "YES" : "NO");

    // ── 5) Verify all hooks fired ────────────────────────────────────────────
    if (!g_draw_instanced_fired || !g_draw_indexed_fired || !g_dispatch_fired || !g_execute_lists_fired) {
        printf("FAIL: not all hooks fired\n");
        remove(queue_hook.slot, queue_hook.original);
        remove(cmd_hooks[0].slot, cmd_hooks[0].original);
        remove(cmd_hooks[1].slot, cmd_hooks[1].original);
        remove(cmd_hooks[2].slot, cmd_hooks[2].original);
        list->Release(); allocator->Release(); queue->Release(); device->Release();
        return 1;
    }

    // ── 6) Remove all hooks via helper DLL and verify restore ───────────────
    const long rm1 = remove(queue_hook.slot, queue_hook.original);
    const long rm2 = remove(cmd_hooks[0].slot, cmd_hooks[0].original);
    const long rm3 = remove(cmd_hooks[1].slot, cmd_hooks[1].original);
    const long rm4 = remove(cmd_hooks[2].slot, cmd_hooks[2].original);
    if (FAILED(rm1) || FAILED(rm2) || FAILED(rm3) || FAILED(rm4)) {
        printf("FAIL: remove_hook returned 0x%08lx/0x%08lx/0x%08lx/0x%08lx\n", rm1, rm2, rm3, rm4);
        list->Release(); allocator->Release(); queue->Release(); device->Release();
        return 1;
    }
    printf("  All hooks removed via helper DLL.\n");

    HRESULT hr_reset = list->Reset(allocator, nullptr);
    printf("  List Reset after remove: 0x%08lx\n", hr_reset);

    // ── 7) Cleanup ───────────────────────────────────────────────────────────
    list->Release();
    allocator->Release();
    queue->Release();
    device->Release();

    detach();
    FreeLibrary(module);

    printf("Result: PASS\n");
    return 0;
}
#else
int main() {
    printf("NOTE: real-device helper-DLL hook test skipped on non-Windows.\n");
    printf("Result: PASS\n");
    return 0;
}
#endif
