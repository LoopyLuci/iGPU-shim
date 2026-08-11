/**
 * @file synapse_d3d12_helper_dll.h
 * @brief External helper DLL interface for D3D12 interception.
 *
 * Provides real hook installation via function-pointer replacement and
 * exported lifecycle APIs usable from a separate helper DLL.
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
 * @brief Install a hook by replacing the function pointer at @p target
 * with @p replacement and saving the original into @p out_original.
 *
 * Returns S_OK on success or a failure HRESULT.
 */
long install_hook(void* target, void* replacement, void** out_original) noexcept;

/**
 * @brief Remove a previously installed hook, restoring @p original.
 */
long remove_hook(void* target, void* original) noexcept;

/**
 * @brief Exported entry points for loader/external consumers.
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
