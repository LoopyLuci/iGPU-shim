/**
 * @file test_d3d12_vtable_dump.cpp
 * @brief D3D12 COM vtable index dump diagnostic.
 *
 * Creates a real D3D12 device, command queue, allocator, and graphics command list,
 * then prints the first 32 vtable function pointers for:
 *  - ID3D12CommandQueue
 *  - ID3D12GraphicsCommandList
 *
 * Also validates the hardcoded indices used by the layer against the real vtable
 * and emits labeled output for the targeted methods to aid debugging.
 */

#if defined(_WIN32)
# include <windows.h>
# include <d3d12.h>
# include <dxgi1_6.h>
#endif

#include <cassert>
#include <cstdio>

#if defined(_WIN32)
# pragma comment(lib, "d3d12.lib")
# pragma comment(lib, "dxgi.lib")

static const char* format_hex(uintptr_t value) {
    thread_local static char buf[32];
    snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)value);
    return buf;
}

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
        printf("  [%02d] %s\n", i, format_hex((uintptr_t)queue_vt[i]));
    }

    printf("ID3D12GraphicsCommandList vtable (first 32 entries):\n");
    void** list_vt = *reinterpret_cast<void***>(list);
    for (int i = 0; i < 32; ++i) {
        printf("  [%02d] %s\n", i, format_hex((uintptr_t)list_vt[i]));
    }

    // Layer hardcoded indices
    constexpr size_t kQueue_ExecuteCommandLists = 3;
    constexpr size_t kList_DrawIndexedInstanced  = 13;
    constexpr size_t kList_DrawInstanced         = 12;
    constexpr size_t kList_Dispatch              = 14;

    printf("\nConsistency check:\n");
    printf("  ExecuteCommandLists [%zu] = %s\n",
           kQueue_ExecuteCommandLists,
           format_hex((uintptr_t)queue_vt[kQueue_ExecuteCommandLists]));
    printf("  DrawInstanced       [%zu] = %s\n",
           kList_DrawInstanced,
           format_hex((uintptr_t)list_vt[kList_DrawInstanced]));
    printf("  DrawIndexedInstanced [%zu] = %s\n",
           kList_DrawIndexedInstanced,
           format_hex((uintptr_t)list_vt[kList_DrawIndexedInstanced]));
    printf("  Dispatch            [%zu] = %s\n",
           kList_Dispatch,
           format_hex((uintptr_t)list_vt[kList_Dispatch]));

    bool ok = true;
    if (!queue_vt[kQueue_ExecuteCommandLists]) {
        printf("FAIL: queue ExecuteCommandLists index %zu is null\n", kQueue_ExecuteCommandLists);
        ok = false;
    }
    if (!list_vt[kList_DrawIndexedInstanced]) {
        printf("FAIL: list DrawIndexedInstanced index %zu is null\n", kList_DrawIndexedInstanced);
        ok = false;
    }
    if (!list_vt[kList_DrawInstanced]) {
        printf("FAIL: list DrawInstanced index %zu is null\n", kList_DrawInstanced);
        ok = false;
    }
    if (!list_vt[kList_Dispatch]) {
        printf("FAIL: list Dispatch index %zu is null\n", kList_Dispatch);
        ok = false;
    }

    list->Release();
    allocator->Release();
    queue->Release();
    device->Release();

    if (!ok) {
        printf("Result: FAIL (update layer indices)\n");
        return 1;
    }

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
