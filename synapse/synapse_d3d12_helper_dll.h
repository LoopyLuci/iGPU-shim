/**
 * @file synapse_d3d12_helper_dll.h
 * @brief External helper DLL interface for D3D12 interception.
 *
 * Provides hook-state tracking and simple function-pointer redirection
 * helpers that can be used from a separate helper DLL.
 */

#ifndef SYNAPSE_D3D12_HELPER_DLL_H
#define SYNAPSE_D3D12_HELPER_DLL_H

#include <cstdint>

#if defined(_WIN32)
# include <windows.h>
#endif

namespace synapse::d3d12::helper {

/**
 * @brief Initialize hook tracking.
 */
long initialize() noexcept;

/**
 * @brief Shutdown and restore any tracked hooks.
 */
void shutdown() noexcept;

/**
 * @brief Return whether helper hooks are currently installed.
 */
bool is_ready() noexcept;

/**
 * @brief Exported entry for compatibility with loader/external consumers.
 */
#if defined(_WIN32)
extern "C" __declspec(dllexport) long __stdcall attach_process_hooks();
extern "C" __declspec(dllexport) void __stdcall detach_process_hooks();
#else
long attach_process_hooks();
void detach_process_hooks();
#endif

}  // namespace synapse::d3d12::helper

#endif  // SYNAPSE_D3D12_HELPER_DLL_H
