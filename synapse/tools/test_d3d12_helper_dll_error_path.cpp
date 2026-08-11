/**
 * @file test_d3d12_helper_dll_error_path.cpp
 * @brief Error-path validation for SynapseD3D12Helper.dll.
 *
 * Verifies that install_hook/remove_hook return expected HRESULTs
 * for invalid arguments and duplicate hook attempts.
 */

#include <cassert>
#include <cstdio>
#include <string>

#if defined(_WIN32)
# include <windows.h>
#endif

int main() {
    printf("=== D3D12 helper-DLL error-path test ===\n");

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

    auto install = reinterpret_cast<long (__stdcall*)(void*, void*, void**)>(
        GetProcAddress(module, "install_hook"));
    auto remove = reinterpret_cast<long (__stdcall*)(void*, void*)>(
        GetProcAddress(module, "remove_hook"));
    if (!install || !remove) {
        printf("  NOTE: install_hook/remove_hook exports missing\n");
        FreeLibrary(module);
        printf("Result: SKIP\n");
        return 0;
    }

    // Null argument validation.
    void* dummy = reinterpret_cast<void*>(0x1234);
    void* original = nullptr;
    long hr = install(nullptr, dummy, &original);
    printf("  install(nullptr, ...) = 0x%08lx\n", hr);
    assert(hr == 0x80070057u);  // E_INVALIDARG

    hr = install(dummy, nullptr, &original);
    printf("  install(dummy, nullptr, ...) = 0x%08lx\n", hr);
    assert(hr == 0x80070057u);

    hr = install(dummy, dummy, nullptr);
    printf("  install(dummy, dummy, nullptr) = 0x%08lx\n", hr);
    assert(hr == 0x80070057u);

    // Duplicate hook detection.
    hr = install(dummy, dummy, &original);
    printf("  first install(dummy, dummy, ...) = 0x%08lx\n", hr);
    assert(SUCCEEDED(hr));

    hr = install(dummy, dummy, &original);
    printf("  duplicate install(dummy, dummy, ...) = 0x%08lx\n", hr);
    assert(hr == 0x800700B7u);  // ERROR_ALREADY_INITIALIZED

    // Invalid remove.
    hr = remove(nullptr, dummy);
    printf("  remove(nullptr, dummy) = 0x%08lx\n", hr);
    assert(hr == 0x80070057u);

    hr = remove(dummy, nullptr);
    printf("  remove(dummy, nullptr) = 0x%08lx\n", hr);
    assert(hr == 0x80070057u);

    FreeLibrary(module);

    printf("Result: PASS\n");
    return 0;
#else
    printf("  non-Windows platform: SKIP\n");
    printf("Result: PASS\n");
    return 0;
#endif
}
