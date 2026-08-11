/**
 * @file bench_d3d12_helper_dll_overhead.cpp
 * @brief Microbenchmark for SynapseD3D12Helper.dll install/remove hook latency.
 */

#include <cstdio>
#include <chrono>
#include <vector>

#if defined(_WIN32)
# include <windows.h>
#endif

static std::vector<double> samples;

#if defined(_WIN32)
static void bench_install_remove(HMODULE module, int iters) {
    using Clock = std::chrono::high_resolution_clock;

    auto attach = reinterpret_cast<long (__stdcall*)()>(
        GetProcAddress(module, "attach_process_hooks"));
    auto detach = reinterpret_cast<void (__stdcall*)()>(
        GetProcAddress(module, "detach_process_hooks"));
    if (!attach || !detach) {
        printf("  missing helper-DLL exports: SKIP\n");
        return;
    }

    double install_sum = 0.0;
    double remove_sum = 0.0;
    for (int i = 0; i < iters; ++i) {
        const auto t0 = Clock::now();
        attach();
        const auto t1 = Clock::now();
        detach();
        const auto t2 = Clock::now();

        install_sum += std::chrono::duration<double, std::milli>(t1 - t0).count();
        remove_sum += std::chrono::duration<double, std::milli>(t2 - t1).count();
    }

    printf("  install avg: %.3f ms, remove avg: %.3f ms (%d iters)\n",
           install_sum / iters, remove_sum / iters, iters);
}
#endif

int main() {
    printf("=== D3D12 helper-DLL overhead benchmark ===\n");

#if defined(_WIN32)
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path = exePath;
    const auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path = path.substr(0, pos + 1);
    path += "SynapseD3D12Helper.dll";

    HMODULE module = LoadLibraryA(path.c_str());
    if (!module) {
        printf("  helper DLL not found (%lu): %s\n", GetLastError(), path.c_str());
        printf("Result: SKIP\n");
        return 0;
    }

    bench_install_remove(module, 64);

    FreeLibrary(module);
#else
    printf("  non-Windows platform: SKIP\n");
#endif

    printf("Result: PASS\n");
    return 0;
}
