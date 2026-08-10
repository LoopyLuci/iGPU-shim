/**
 * @file synapse_d3d12_layer.h
 * @brief D3D12 interception stub — placeholder for D3D12 backend work.
 *
 * This is a scaffolding header only. No interception is implemented yet.
 *
 * Planned interception points:
 * - ID3D12CommandQueue::ExecuteCommandLists
 * - ID3D12GraphicsCommandList::DrawIndexedInstanced / DrawInstanced
 * - ID3D12GraphicsCommandList::Dispatch
 * - ID3D12Device::CreateCommandQueue / CreateCommandList
 */

#ifndef SYNAPSE_D3D12_LAYER_H
#define SYNAPSE_D3D12_LAYER_H

#include <unknwn.h>

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

} // namespace synapse::d3d12

#endif // SYNAPSE_D3D12_LAYER_H
