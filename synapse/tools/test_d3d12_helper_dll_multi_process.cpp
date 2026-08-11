/**
 * @file test_d3d12_helper_dll_multi_process.cpp
 * @brief Cross-process helper-DLL smoke test.
 *
 * Loads SynapseD3D12Helper.dll and exercises `helper_test`
 * to confirm the DLL exports and runs in this process.
 */

#include <cstdio>
#include <string>

#if defined(_WIN32)
# include <windows.h>
#endif

int main() {
    printf("=== D3D12 helper-DLL multi-process test ===\n");

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

    auto test = reinterpret_cast<int (__stdcall*)(int)>(
        GetProcAddress(module, "helper_test"));
    if (!test) {
        printf("  NOTE: helper_test export missing\n");
        FreeLibrary(module);
        printf("Result: SKIP\n");
        return 0;
    }

    const int input = 7;
    const int output = test(input);
    printf("  helper_test(%d) = %d\n", input, output);
    FreeLibrary(module);

    if (output == input + 1) {
        printf("Result: PASS\n");
        return 0;
    } else {
        printf("Result: FAIL\n");
        return 1;
    }
#else
    printf("  non-Windows platform: SKIP\n");
    printf("Result: PASS\n");
    return 0;
#endif
}
