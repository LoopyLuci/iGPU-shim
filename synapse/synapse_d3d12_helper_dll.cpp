/**
 * @file synapse_d3d12_helper_dll.cpp
 * @brief Minimal Windows helper DLL entry points for D3D12 hooking.
 *
 * This DLL is intentionally self-contained and does not pull d3d12.h
 * into the main layer build. It exposes exported hook lifecycle APIs.
 */

#include "synapse_d3d12_helper_dll.h"

#include <cstring>
#include <mutex>

namespace synapse::d3d12::helper {

namespace {

std::once_flag g_init_once;
long g_init_hr = S_OK;
bool g_ready = false;
std::mutex g_mutex;

}  // anonymous namespace

long initialize() noexcept {
    std::call_once(g_init_once, []() { g_init_hr = S_OK; });
    return g_init_hr;
}

void shutdown() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ready = false;
}

bool is_ready() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_ready;
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
