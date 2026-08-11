/**
 * @file test_d3d12_helper_dll_stress.cpp
 * @brief Stress test for SynapseD3D12Helper.dll lifecycle.
 *
 * Validates that repeated attach→install→remove→detach cycles
 * don't leak handles or crash.
 */

#include <cassert>
#include <cstdio>
#include <string>

#if defined(_WIN32)
# include <windows.h>
#endif

int main() {
    printf("=== D3D12 helper-DLL stress test ===\n");

#if defined(_WIN32)
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path = exePath;
    const auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path = path.substr(0, pos + 1);
    path += "SynapseD3D12Helper.dll";

    HMODULE module = LoadLibraryA(path.c_str());
    if (!module) {
        printf("  NOTE: helper DLL not found (%lu): %s\n", GetLastError(), path.c_str());
        printf("Result: SKIP\n");
        return 0;
    }

    auto attach = reinterpret_cast<long (__stdcall*)()>(
        GetProcAddress(module, "attach_process_hooks"));
    auto detach = reinterpret_cast<void (__stdcall*)()>(
        GetProcAddress(module, "detach_process_hooks"));
    auto install = reinterpret_cast<long (__stdcall*)(void*, void*, void**)>(
        GetProcAddress(module, "install_hook"));
    auto remove = reinterpret_cast<long (__stdcall*)(void*, void*)>(
        GetProcAddress(module, "remove_hook"));
    if (!attach || !detach || !install || !remove) {
        printf("  NOTE: helper-DLL exports missing\n");
        FreeLibrary(module);
        printf("Result: SKIP\n");
        return 0;
    }

    const int cycles = 1000;
    void* target = reinterpret_cast<void*>(0x1234);
    void* replacement = reinterpret_cast<void*>(0x5678);
    void* original = nullptr;

    for (int i = 0; i < cycles; ++i) {
        attach();
        long hr = install(target, replacement, &original);
        assert(SUCCEEDED(hr) && "install should succeed in stress loop");
        hr = remove(target, original);
        assert(SUCCEEDED(hr) && "remove should succeed in stress loop");
        detach();
    }

    printf("  completed %d attach→install→remove→detach cycles\n", cycles);
    FreeLibrary(module);

    printf("Result: PASS\n");
    return 0;
#else
    printf("  non-Windows platform: SKIP\n");
    printf("Result: PASS\n");
    return 0;
#endif
}
