/**
 * @file synapse_d3d12_layer.h
 * @brief D3D12 interception stub — placeholder for D3D12 backend work.
 *
 * This scaffolding declares the COM entry points we intend to intercept
 * and a minimal init/shutdown pair. No interception is implemented yet.
 */

#ifndef SYNAPSE_D3D12_LAYER_H
#define SYNAPSE_D3D12_LAYER_H

#include <unknwn.h>
#include <cstdint>

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
 * @brief Planned interception points for future implementation.
 */
namespace planned {
    struct CommandQueue {
        virtual void ExecuteCommandLists(
            uint32_t NumLists,
            ID3D12CommandList* const* ppCommandLists) = 0;
    };

    struct GraphicsCommandList {
        virtual void DrawIndexedInstanced(
            uint32_t IndexCountPerInstance,
            uint32_t InstanceCount,
            uint32_t StartIndexLocation,
            int32_t BaseVertexLocation,
            uint32_t StartInstanceLocation) = 0;

        virtual void DrawInstanced(
            uint32_t VertexCountPerInstance,
            uint32_t InstanceCount,
            uint32_t StartVertexLocation,
            uint32_t StartInstanceLocation) = 0;

        virtual void Dispatch(
            uint32_t ThreadGroupCountX,
            uint32_t ThreadGroupCountY,
            uint32_t ThreadGroupCountZ) = 0;
    };
}

} // namespace synapse::d3d12

#endif // SYNAPSE_D3D12_LAYER_H

