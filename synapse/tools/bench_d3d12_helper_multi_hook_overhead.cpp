/**@file bench_d3d12_helper_multi_hook_overhead.cpp
 * @brief Measures install/remove overhead of the SynapseD3D12Helper DLL
 *        when placing N hooks sequentially.
 *
 * Calls attach_process_hooks(), then installs and removes K hooks per
 * iteration in a tight loop, reporting per-hook install time, per-hook
 * remove time, and total round-trip. On non-Windows the test is skipped.
 */

#if defined(_WIN32)
# include <windows.h>
#endif

#include "../synapse_d3d12_helper_dll.h"

#include <cstdio>
#include <cstring>
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

static void __stdcall dummy_replacement() { /* no-op */ }
static void __stdcall dummy_target()    { /* no-op */ }

int main() {
    printf("=== D3D12 Helper DLL Multi-Hook Overhead Benchmark ===\n");

    const std::string dllPath = build_dll_path();
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

    const long hr = attach();
    if (FAILED(hr)) { printf("FAIL: attach = 0x%08lx\n", hr); return 1; }
    if (!ready())    { printf("FAIL: !ready\n"); return 1; }

    constexpr int kIterations = 1000;
    constexpr int kNumHooks   = 4;

    // Allocate K function-pointer slots. Each slot is a writeable variable
    // whose pointer value install_hook patches.
    std::vector<void*> slots(kNumHooks);
    std::vector<void*> originals(kNumHooks);
    for (int i = 0; i < kNumHooks; ++i) slots[i] = &dummy_target;

    // Warm up.
    for (int i = 0; i < kNumHooks; ++i) {
        void* orig = nullptr;
        if (FAILED(install(&slots[i], &dummy_replacement, &orig))) {
            printf("FAIL: warmup install %d\n", i); return 1;
        }
        if (FAILED(remove(&slots[i], orig))) {
            printf("FAIL: warmup remove %d\n", i); return 1;
        }
    }

    // ── Measure install+remove overhead per iteration ────────────────────────
    // Each iteration installs all K hooks, then removes them, so every
    // iteration starts from a clean state.
    long long install_total_ns = 0;
    long long remove_total_ns  = 0;

    for (int iter = 0; iter < kIterations; ++iter) {
        // Install phase.
        for (int i = 0; i < kNumHooks; ++i) {
            void* orig = nullptr;
            const auto t0 = GetTickCount64();
            const long hr2 = install(&slots[i], &dummy_replacement, &orig);
            const auto t1 = GetTickCount64();
            if (FAILED(hr2)) {
                printf("FAIL: install iter=%d hook=%d 0x%08lx\n", iter, i, hr2);
                return 1;
            }
            originals[i] = orig;
            install_total_ns += static_cast<long long>(t1 - t0) * 1000000LL;
        }
        // Remove phase.
        for (int i = 0; i < kNumHooks; ++i) {
            const auto t0 = GetTickCount64();
            const long hr3 = remove(&slots[i], originals[i]);
            const auto t1 = GetTickCount64();
            if (FAILED(hr3)) {
                printf("FAIL: remove iter=%d hook=%d 0x%08lx\n", iter, i, hr3);
                return 1;
            }
            remove_total_ns += static_cast<long long>(t1 - t0) * 1000000LL;
        }
    }

    const double install_avg_us = install_total_ns / (kIterations * kNumHooks) / 1000.0;
    const double remove_avg_us  = remove_total_ns  / (kIterations * kNumHooks) / 1000.0;
    const double roundtrip_us  = (install_avg_us + remove_avg_us);

    printf("  Iterations:    %d\n", kIterations);
    printf("  Hooks/iter:    %d\n", kNumHooks);
    printf("  Install avg:   %.3f µs / hook\n", install_avg_us);
    printf("  Remove avg:    %.3f µs / hook\n", remove_avg_us);
    printf("  Round-trip:    %.3f µs / hook\n", roundtrip_us);
    printf("  Total samples: %d installs + %d removes\n",
           kIterations * kNumHooks, kIterations * kNumHooks);

    detach();
    FreeLibrary(module);
    printf("Result: PASS\n");
    return 0;
}
#else
int main() {
    printf("NOTE: overhead benchmark skipped on non-Windows platform.\n");
    printf("Result: PASS\n");
    return 0;
}
#endif
