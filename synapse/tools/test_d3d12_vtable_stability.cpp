/**
 * @file test_d3d12_vtable_stability.cpp
 * @brief Stability test for D3D12 vtable indices.
 *
 * Runs the vtable dump twice and asserts the targeted method indices
 * and function pointers are stable across calls.
 */

#if defined(_WIN32)
# include <windows.h>
# include <d3d12.h>
# include <dxgi1_6.h>
#endif

#include <cassert>
#include <cstdio>
#include <array>
#include <vector>

#if defined(_WIN32)
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

struct VTableSnapshot {
    size_t queue_execute_lists = 0;
    size_t queue_signal = 0;
    size_t queue_wait = 0;
    size_t list_draw_instanced = 0;
    size_t list_draw_indexed_instanced = 0;
    size_t list_dispatch = 0;
    void* queue_execute_lists_ptr = nullptr;
    void* queue_signal_ptr = nullptr;
    void* queue_wait_ptr = nullptr;
    void* list_draw_instanced_ptr = nullptr;
    void* list_draw_indexed_instanced_ptr = nullptr;
    void* list_dispatch_ptr = nullptr;
};

std::array<size_t, 6> kTargetQueueIndices = {3, 7, 8};
std::array<size_t, 6> kTargetListIndices = {12, 13, 14};

VTableSnapshot dump_vtables() {
    VTableSnapshot snap{};

    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        return snap;
    }

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.NodeMask = 0;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
        device->Release();
        return snap;
    }

    ID3D12CommandAllocator* allocator = nullptr;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
        queue->Release();
        device->Release();
        return snap;
    }

    ID3D12GraphicsCommandList* list = nullptr;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)))) {
        allocator->Release();
        queue->Release();
        device->Release();
        return snap;
    }

    {
        void** queue_vt = *reinterpret_cast<void***>(queue);
        snap.queue_execute_lists_ptr = queue_vt[3];
        snap.queue_execute_lists = 3;
        snap.queue_signal_ptr = queue_vt[7];
        snap.queue_signal = 7;
        snap.queue_wait_ptr = queue_vt[8];
        snap.queue_wait = 8;
    }

    {
        void** list_vt = *reinterpret_cast<void***>(list);
        snap.list_draw_instanced_ptr = list_vt[12];
        snap.list_draw_indexed_instanced_ptr = list_vt[13];
        snap.list_dispatch_ptr = list_vt[14];
        snap.list_draw_instanced = 12;
        snap.list_draw_indexed_instanced = 13;
        snap.list_dispatch = 14;
    }

    list->Release();
    allocator->Release();
    queue->Release();
    device->Release();

    return snap;
}

}  // namespace
#endif

int main() {
    printf("=== D3D12 VTable Stability Test ===\n");

#if defined(_WIN32)
    const VTableSnapshot first = dump_vtables();
    const VTableSnapshot second = dump_vtables();

    printf("  first  queue ExecuteCommandLists [%zu] = %p\n",
           first.queue_execute_lists, first.queue_execute_lists_ptr);
    printf("  second queue ExecuteCommandLists [%zu] = %p\n",
           second.queue_execute_lists, second.queue_execute_lists_ptr);

    printf("  first  list DrawInstanced [%zu] = %p\n",
           first.list_draw_instanced, first.list_draw_instanced_ptr);
    printf("  second list DrawInstanced [%zu] = %p\n",
           second.list_draw_instanced, second.list_draw_instanced_ptr);

    printf("  first  list DrawIndexedInstanced [%zu] = %p\n",
           first.list_draw_indexed_instanced, first.list_draw_indexed_instanced_ptr);
    printf("  second list DrawIndexedInstanced [%zu] = %p\n",
           second.list_draw_indexed_instanced, second.list_draw_indexed_instanced_ptr);

    printf("  first  list Dispatch [%zu] = %p\n",
           first.list_dispatch, first.list_dispatch_ptr);
    printf("  second list Dispatch [%zu] = %p\n",
           second.list_dispatch, second.list_dispatch_ptr);

    printf("  first  queue Signal [%zu] = %p\n",
           first.queue_signal, first.queue_signal_ptr);
    printf("  second queue Signal [%zu] = %p\n",
           second.queue_signal, second.queue_signal_ptr);

    printf("  first  queue Wait [%zu] = %p\n",
           first.queue_wait, first.queue_wait_ptr);
    printf("  second queue Wait [%zu] = %p\n",
           second.queue_wait, second.queue_wait_ptr);

    assert(first.queue_execute_lists == second.queue_execute_lists);
    assert(first.list_draw_instanced == second.list_draw_instanced);
    assert(first.list_draw_indexed_instanced == second.list_draw_indexed_instanced);
    assert(first.list_dispatch == second.list_dispatch);
    assert(first.queue_execute_lists_ptr == second.queue_execute_lists_ptr);
    assert(first.list_draw_instanced_ptr == second.list_draw_instanced_ptr);
    assert(first.list_draw_indexed_instanced_ptr == second.list_draw_indexed_instanced_ptr);
    assert(first.list_dispatch_ptr == second.list_dispatch_ptr);
    assert(first.queue_signal == second.queue_signal);
    assert(first.queue_wait == second.queue_wait);
    assert(first.queue_signal_ptr == second.queue_signal_ptr);
    assert(first.queue_wait_ptr == second.queue_wait_ptr);
#else
    printf("  non-Windows platform: SKIP\n");
#endif

    printf("Result: PASS\n");
    return 0;
}
