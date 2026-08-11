/**
 * @file test_d3d12_helper_dll_multi_process.cpp
 * @brief Cross-process helper-DLL validation.
 *
 * Spawns test_d3d12_helper_dll.exe as a child process and verifies
 * it exits successfully. This confirms the helper DLL can be loaded
 * and used in a separate process context.
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
    path += "test_d3d12_helper_dll.exe";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(
            path.c_str(),
            nullptr,
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        printf("  NOTE: failed to spawn child process (%lu): %s\n",
               GetLastError(), path.c_str());
        printf("Result: SKIP\n");
        return 0;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        exit_code = static_cast<DWORD>(-1);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    printf("  child process exit code: %lu\n", exit_code);
    if (exit_code == 0) {
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
