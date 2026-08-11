/**
 * @file synapse_d3d12_helper_dll.cpp
 * @brief Minimal Windows helper DLL entry points for D3D12 hooking.
 *
 * This DLL is intentionally self-contained and does not pull d3d12.h
 * into the main layer build. It provides real function-pointer replacement
 * hooks for interception.
 */

#include "synapse_d3d12_helper_dll.h"

#include <cstring>
#include <mutex>
#include <winerror.h>

namespace synapse::d3d12::helper {

namespace {

struct HookRecord {
    void* target;
    void* original;
    void* replacement;
    bool active;
};

std::once_flag g_init_once;
long g_init_hr = S_OK;
bool g_ready = false;
std::mutex g_mutex;
HookRecord g_hooks[8];
size_t g_hook_count = 0;

bool replace_function_pointer(void* target, void* replacement, void** out_original) {
    DWORD old_protect = 0;
    if (!VirtualProtect(target, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }

    void* original = *static_cast<void**>(target);
    *static_cast<void**>(target) = replacement;
    *out_original = original;

    DWORD ignored = 0;
    VirtualProtect(target, sizeof(void*), old_protect, &ignored);
    return true;
}

bool restore_function_pointer(void* target, void* original) {
    DWORD old_protect = 0;
    if (!VirtualProtect(target, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }

    *static_cast<void**>(target) = original;

    DWORD ignored = 0;
    VirtualProtect(target, sizeof(void*), old_protect, &ignored);
    return true;
}

HookRecord* find_hook(void* target) {
    for (size_t i = 0; i < g_hook_count; ++i) {
        if (g_hooks[i].target == target) {
            return &g_hooks[i];
        }
    }
    return nullptr;
}

}  // anonymous namespace

long initialize() noexcept {
    std::call_once(g_init_once, []() { g_init_hr = S_OK; });
    return g_init_hr;
}

void shutdown() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_hook_count; ++i) {
        if (g_hooks[i].active) {
            restore_function_pointer(g_hooks[i].target, g_hooks[i].original);
            g_hooks[i].active = false;
        }
    }
    g_hook_count = 0;
    g_ready = false;
}

bool is_ready() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_ready;
}

long install_hook(void* target, void* replacement, void** out_original) noexcept {
    if (!target || !replacement || !out_original) {
        return E_INVALIDARG;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (find_hook(target)) {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    }
    if (g_hook_count >= sizeof(g_hooks) / sizeof(g_hooks[0])) {
        return E_OUTOFMEMORY;
    }

    void* original = nullptr;
    if (!replace_function_pointer(target, replacement, &original)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    g_hooks[g_hook_count++] = {target, original, replacement, true};
    g_ready = true;
    *out_original = original;
    return S_OK;
}

long remove_hook(void* target, void* original) noexcept {
    if (!target || !original) {
        return E_INVALIDARG;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    HookRecord* record = find_hook(target);
    if (!record || record->original != original) {
        return E_INVALIDARG;
    }

    if (!restore_function_pointer(target, original)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    record->active = false;
    g_ready = (g_hook_count > 1);
    if (g_hook_count == 1) {
        g_hook_count = 0;
    }
    return S_OK;
}

extern "C" __declspec(dllexport) long __stdcall attach_process_hooks() {
    initialize();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ready = true;
    return g_init_hr;
}

extern "C" __declspec(dllexport) void __stdcall detach_process_hooks() {
    shutdown();
}

}  // namespace synapse::d3d12::helper
