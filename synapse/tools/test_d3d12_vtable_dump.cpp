/**
 * @file test_d3d12_vtable_dump.cpp
 * @brief D3D12 COM vtable index dump diagnostic.
 *
 * Creates a real D3D12 device, command queue, allocator, and graphics command list,
 * then prints the first 32 vtable function pointers for:
 *  - ID3D12CommandQueue
 *  - ID3D12GraphicsCommandList
 *
 * This is used to discover real vtable indices on the current Windows SDK / driver,
 * because hardcoded indices vary across SDK versions.
 */

#if defined(_WIN32)
# include <windows.h>
# include <d3d12.h>
# include <dxgi1_6.h>
#endif

#include <cstdio>

#if defined(_WIN32)
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

int main() {
    printf("=== D3D12 VTable Dump ===\n");

    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        printf("NOTE: D3D12 device creation failed with 0x%08lx; skipping.\n", hr);
        printf("Result: PASS\n");
        return 0;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.NodeMask = 0;

    ID3D12CommandQueue* queue = nullptr;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr) || !queue) {
        device->Release();
        printf("NOTE: command queue creation failed with 0x%08lx; skipping.\n", hr);
        printf("Result: PASS\n");
        return 0;
    }

    ID3D12CommandAllocator* allocator = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr) || !allocator) {
        queue->Release();
        device->Release();
        printf("NOTE: command allocator creation failed with 0x%08lx; skipping.\n", hr);
        printf("Result: PASS\n");
        return 0;
    }

    ID3D12GraphicsCommandList* list = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (FAILED(hr) || !list) {
        allocator->Release();
        queue->Release();
        device->Release();
        printf("NOTE: command list creation failed with 0x%08lx; skipping.\n", hr);
        printf("Result: PASS\n");
        return 0;
    }

    printf("ID3D12CommandQueue vtable (first 16 entries):\n");
    void** queue_vt = *reinterpret_cast<void***>(queue);
    for (int i = 0; i < 16; ++i) {
        printf("  [%02d] %p\n", i, queue_vt[i]);
    }

    printf("ID3D12GraphicsCommandList vtable (first 32 entries):\n");
    void** list_vt = *reinterpret_cast<void***>(list);
    for (int i = 0; i < 32; ++i) {
        printf("  [%02d] %p\n", i, list_vt[i]);
    }

    list->Release();
    allocator->Release();
    queue->Release();
    device->Release();

    printf("Result: PASS\n");
    return 0;
}
#else
int main() {
    printf("NOTE: D3D12 vtable dump skipped on non-Windows platform.\n");
    printf("Result: PASS\n");
    return 0;
}
#endif
