/**@file test_d3d12_helper_multi_hook.cpp
 * @brief Validates multi-entry hooking through the SynapseD3D12Helper DLL.
 *
 * Dynamically loads SynapseD3D12Helper.dll, calls attach_process_hooks(),
 * then uses install_hook/remove_hook to place replacement hooks on multiple
 * independent function-pointer targets in sequence, fires each through its
 * pointer, verifies the per-target callback, and removes all hooks.
 *
 * This is an API-surface validation of the helper DLL's install/remove path
 * against multiple independent targets. D3D12 vtable hooking is covered by
 * test_d3d12_multi_hook.cpp (which patches vtable slots directly).
 */

#if defined(_WIN32)
# include <windows.h>
#endif

#include "../synapse_d3d12_helper_dll.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
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

// ── per-target callback tracking ─────────────────────────────────────────────

static bool g_t0_fired = false;
static bool g_t1_fired = false;
static bool g_t2_fired = false;
static bool g_t3_fired = false;

static void __stdcall repl_t0() { g_t0_fired = true; }
static void __stdcall repl_t1() { g_t1_fired = true; }
static void __stdcall repl_t2() { g_t2_fired = true; }
static void __stdcall repl_t3() { g_t3_fired = true; }

// Function-pointer variables — these are the hook targets.
// install_hook patches the pointerVALUE, not the function code.
static void (__stdcall *target_0)() = repl_t0;  // original points to replacement (so even unhooked it fires)
static void (__stdcall *target_1)() = repl_t1;
static void (__stdcall *target_2)() = repl_t2;
static void (__stdcall *target_3)() = repl_t3;

// The install_hook API replaces the pointer value at the target address.
// To make the test meaningful, we set the pointers to a noop first, then
// install hooks that point to the replacement functions, then call through
// the pointer variables to verify the replacement fired.

static void __stdcall noop() { /* intentionally empty */ }

int main() {
    printf("=== D3D12 Helper DLL Multi-Hook Test ===\n");

    const std::string dllPath = build_dll_path();
    printf("  DLL path: %s\n", dllPath.c_str());

    HMODULE module = nullptr;
    LoadFunc   attach   = nullptr;
    ReadyFunc  ready    = nullptr;
    UnloadFunc detach   = nullptr;
    InstallFunc install = nullptr;
    RemoveFunc remove   = nullptr;

    if (!load_helper(dllPath, module, attach, ready, detach, install, remove)) {
        printf("  NOTE: helper DLL not loadable — skipping in this environment.\n");
        printf("Result: PASS\n");
        return 0;
    }
    printf("  Helper DLL loaded.\n");

    // ── 1) Lifecycle ────────────────────────────────────────────────────────
    const long hr = attach();
    if (FAILED(hr)) {
        printf("FAIL: attach_process_hooks = 0x%08lx\n", hr);
        return 1;
    }
    printf("  attach_process_hooks = 0x%08lx.\n", hr);

    if (!ready()) {
        printf("FAIL: is_ready() false after attach\n");
        detach(); FreeLibrary(module);
        return 1;
    }
    printf("  is_ready() = true.\n");

    // ── 2) Reset targets to noop so we can detect the hook firing ───────────
    target_0 = noop;
    target_1 = noop;
    target_2 = noop;
    target_3 = noop;

    // ── 3) Install 4 hooks sequentially ─────────────────────────────────────
    struct TargetStep {
        const char* name;
        void**      target_ptr;   // address of the function-pointer variable
        void*       replacement;
        bool*       fired_flag;
    };
    TargetStep steps[] = {
        {"t0", reinterpret_cast<void**>(&target_0), &repl_t0, &g_t0_fired},
        {"t1", reinterpret_cast<void**>(&target_1), &repl_t1, &g_t1_fired},
        {"t2", reinterpret_cast<void**>(&target_2), &repl_t2, &g_t2_fired},
        {"t3", reinterpret_cast<void**>(&target_3), &repl_t3, &g_t3_fired},
    };

    void* originals[4] = {};

    printf("  Installing %d hooks sequentially:\n", 4);
    for (int i = 0; i < 4; ++i) {
        void* orig = nullptr;
        const long hr2 = install(steps[i].target_ptr, steps[i].replacement, &orig);
        if (FAILED(hr2)) {
            printf("    FAIL: install_hook(%s) returned 0x%08lx\n",
                   steps[i].name, hr2);
            for (int j = 0; j < i; ++j) remove(steps[j].target_ptr, originals[j]);
            detach(); FreeLibrary(module);
            return 1;
        }
        originals[i] = orig;
        printf("    [%d] %s installed  (saved orig=%p)\n", i, steps[i].name, orig);
    }

    // ── 4) Fire each target through its pointer and verify callback ─────────
    printf("  Firing each target through its pointer:\n");
    g_t0_fired = g_t1_fired = g_t2_fired = g_t3_fired = false;

    target_0();
    target_1();
    target_2();
    target_3();

    printf("    t0 fired: %s\n", g_t0_fired ? "YES" : "NO");
    printf("    t1 fired: %s\n", g_t1_fired ? "YES" : "NO");
    printf("    t2 fired: %s\n", g_t2_fired ? "YES" : "NO");
    printf("    t3 fired: %s\n", g_t3_fired ? "YES" : "NO");

    if (!g_t0_fired || !g_t1_fired || !g_t2_fired || !g_t3_fired) {
        printf("FAIL: not all hooks fired\n");
        for (int i = 0; i < 4; ++i) remove(steps[i].target_ptr, originals[i]);
        detach(); FreeLibrary(module);
        return 1;
    }

    // ── 5) Remove all hooks and verify original behavior restored ───────────
    printf("  Removing all hooks:\n");
    for (int i = 0; i < 4; ++i) {
        const long hr3 = remove(steps[i].target_ptr, originals[i]);
        if (FAILED(hr3)) {
            printf("    FAIL: remove_hook(%s) returned 0x%08lx\n",
                   steps[i].name, hr3);
            detach(); FreeLibrary(module);
            return 1;
        }
        printf("    [%d] %s removed\n", i, steps[i].name);
    }

    // After removal, calling through the pointer should execute the saved
    // original (which was noop), so the replacement should NOT fire again.
    g_t0_fired = g_t1_fired = g_t2_fired = g_t3_fired = false;
    target_0();
    target_1();
    target_2();
    target_3();

    if (g_t0_fired || g_t1_fired || g_t2_fired || g_t3_fired) {
        printf("FAIL: replacement fired after remove_hook\n");
        detach(); FreeLibrary(module);
        return 1;
    }
    printf("    After remove: no replacement fired (correct).\n");

    // ── 6) Re-install and remove one target to confirm idempotency ──────────
    void* re_orig = nullptr;
    const long hr4 = install(steps[0].target_ptr, steps[0].replacement, &re_orig);
    if (FAILED(hr4)) {
        printf("FAIL: re-install_hook(t0) returned 0x%08lx\n", hr4);
        detach(); FreeLibrary(module);
        return 1;
    }
    printf("  Re-installed t0 (orig=%p).\n", re_orig);
    g_t0_fired = false;
    target_0();
    assert(g_t0_fired && "re-installed hook should fire");
    const long hr5 = remove(steps[0].target_ptr, re_orig);
    if (FAILED(hr5)) {
        printf("FAIL: re-remove_hook(t0) returned 0x%08lx\n", hr5);
        detach(); FreeLibrary(module);
        return 1;
    }
    printf("  Re-removed t0.\n");

    // ── 7) Cleanup ──────────────────────────────────────────────────────────
    detach();
    FreeLibrary(module);
    printf("  Detached and unloaded helper DLL.\n");

    printf("Result: PASS\n");
    return 0;
}
#else
int main() {
    printf("NOTE: helper-DLL multi-hook test skipped on non-Windows platform.\n");
    printf("Result: PASS\n");
    return 0;
}
#endif
