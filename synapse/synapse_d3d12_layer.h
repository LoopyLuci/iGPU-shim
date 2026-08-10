/**
 * @file synapse_d3d12_layer.h
 * @brief D3D12 interception stub — COM vtable interception scaffolding.
 *
 * Declares a minimal vtable-patch pattern and static interceptor helpers for
 * D3D12 command queues / graphics command lists. No interception is active
 * until enable_d3d12_interception(true) is called.
 */

#ifndef SYNAPSE_D3D12_LAYER_H
#define SYNAPSE_D3D12_LAYER_H

#include <cstdint>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
# ifndef _HRESULT_DEFINED
typedef long HRESULT;
#  define S_OK        ((HRESULT)0L)
#  define E_FAIL      ((HRESULT)1L)
#  define E_NOTIMPL   ((HRESULT)0x80004001L)
#  define E_INVALIDARG ((HRESULT)0x80070057L)
#  define E_ABORT     ((HRESULT)0x80004004L)
#  define _HRESULT_DEFINED
# endif
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandList;
#endif

namespace synapse::d3d12 {

/**
 * @brief Initialize D3D12 interception layer.
 * @return S_OK if supported on this OS/hardware, else E_NOTIMPL.
 */
[[nodiscard]] HRESULT initialize() noexcept;

/**
 * @brief Shutdown D3D12 interception layer and release resources.
 */
void shutdown() noexcept;

/**
 * @brief Enable or disable D3D12 interception at runtime.
 * @return S_OK on success, E_FAIL if interception is not available.
 */
[[nodiscard]] HRESULT enable_d3d12_interception(bool enabled) noexcept;

/**
 * @brief Query whether D3D12 interception is currently active.
 */
bool is_d3d12_interception_enabled() noexcept;

/**
 * @brief Hook callback type for intercepted D3D12 commands.
 */
using D3D12Hook = void (*)(void* cmd_object, void* recorded_state);

/**
 * @brief COM vtable interception utilities.
 *
 * Pattern:
 *   1. Patch the desired vtable slot in-place with a trampoline.
 *   2. The trampoline calls the original vtable slot, then invokes user code.
 *   3. restore() reverts the slot when the object's lifetime ends.
 */
namespace vtable {
    /**
     * @brief Patch one vtable slot in-place and return the original entry.
     */
    void* patch(void** object_vtable, size_t index, void* replace) noexcept;

    /**
     * @brief Restore a previously patched vtable slot.
     */
    void restore(void** object_vtable, size_t index, void* original) noexcept;
}

#if defined(_WIN32)

/**
 * @brief Intercept ID3D12CommandQueue::ExecuteCommandLists.
 *
 * Hooks are global; only one active hook is supported in this stub.
 */
struct CommandQueueInterceptor {
    static [[nodiscard]] HRESULT attach(ID3D12CommandQueue* queue,
                                        D3D12Hook hook) noexcept;
    static void detach(ID3D12CommandQueue* queue) noexcept;

    static void on_execute_command_lists(
        ID3D12CommandQueue* queue,
        unsigned int NumLists,
        ID3D12CommandList* const* ppLists) noexcept;
};

/**
 * @brief Intercept graphics command-list draw/dispatch entry points.
 */
struct GraphicsCommandListInterceptor {
    static [[nodiscard]] HRESULT attach(ID3D12GraphicsCommandList* list,
                                        D3D12Hook hook) noexcept;
    static void detach(ID3D12GraphicsCommandList* list) noexcept;

    static void on_draw_indexed_instanced(
        ID3D12GraphicsCommandList* list,
        unsigned int IndexCountPerInstance,
        unsigned int InstanceCount,
        unsigned int StartIndexLocation,
        int BaseVertexLocation,
        unsigned int StartInstanceLocation) noexcept;

    static void on_draw_instanced(
        ID3D12GraphicsCommandList* list,
        unsigned int VertexCountPerInstance,
        unsigned int InstanceCount,
        unsigned int StartVertexLocation,
        unsigned int StartInstanceLocation) noexcept;

    static void on_dispatch(
        ID3D12GraphicsCommandList* list,
        unsigned int ThreadGroupCountX,
        unsigned int ThreadGroupCountY,
        unsigned int ThreadGroupCountZ) noexcept;
};

#endif // _WIN32

} // namespace synapse::d3d12

#endif // SYNAPSE_D3D12_LAYER_H
