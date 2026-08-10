/**
 * @file test_d3d12_interception.cpp
 * @brief Unit tests for D3D12 COM vtable interception scaffolding.
 *
 * Uses a fake COM vtable to verify patch/restore semantics and hook
 * callback dispatch without requiring a real D3D12 device.
 */

#include "synapse_d3d12_layer.h"
#include <cassert>
#include <cstdio>
#include <memory>
#include <new>

#ifdef _WIN32
# include <windows.h>
#endif

namespace synapse::d3d12 {

namespace {

#ifdef _WIN32

struct FakeVTable {
    void* fn0;
    void* fn1;
    void* execute;
    void* draw;
    void* dispatch;
};

struct FakeQueue {
    FakeVTable* vtable;
};

struct FakeList {
    FakeVTable* vtable;
};

int g_hook_calls = 0;

void __cdecl fake_hook(void* /*cmd_object*/, void* /*recorded_state*/) noexcept {
    g_hook_calls++;
}

void __cdecl fake_original(void*) noexcept {
    // Original no-op.
}

FakeVTable g_fake_queue_vt = {nullptr, nullptr, reinterpret_cast<void*>(fake_original), nullptr, nullptr};
FakeVTable g_fake_list_vt  = {nullptr, nullptr, nullptr, reinterpret_cast<void*>(fake_original), reinterpret_cast<void*>(fake_original)};

#else

struct FakeVTable { void* pad; };
struct FakeQueue { FakeVTable* vtable; };
struct FakeList  { FakeVTable* vtable; };

#endif

} // anonymous namespace

int main() {
    printf("=== D3D12 Interception Unit Test ===\n");

#ifdef _WIN32
    assert(initialize() == S_OK);
    assert(enable_d3d12_interception(true) == S_OK);
    assert(is_d3d12_interception_enabled() == true);

    // Test vtable patch/restore.
    FakeQueue queue;
    queue.vtable = &g_fake_queue_vt;
    void* original = vtable::patch(reinterpret_cast<void**>(queue.vtable), 2, reinterpret_cast<void*>(fake_hook));
    assert(original != nullptr);
    assert(queue.vtable->execute == reinterpret_cast<void*>(fake_hook));

    vtable::restore(reinterpret_cast<void**>(queue.vtable), 2, original);
    assert(queue.vtable->execute == original);

    // Test CommandQueueInterceptor attach/detach.
    g_hook_calls = 0;
    assert(CommandQueueInterceptor::attach(reinterpret_cast<ID3D12CommandQueue*>(&queue), fake_hook) == S_OK);
    assert(queue.vtable->execute == reinterpret_cast<void*>(&CommandQueueInterceptor::on_execute_command_lists));

    CommandQueueInterceptor::detach(reinterpret_cast<ID3D12CommandQueue*>(&queue));
    assert(queue.vtable->execute == original);

    // Test GraphicsCommandListInterceptor attach/detach.
    FakeList list;
    list.vtable = &g_fake_list_vt;
    assert(GraphicsCommandListInterceptor::attach(reinterpret_cast<ID3D12GraphicsCommandList*>(&list), fake_hook) == S_OK);
    assert(list.vtable->draw == reinterpret_cast<void*>(&GraphicsCommandListInterceptor::on_draw_indexed_instanced));
    assert(list.vtable->dispatch == reinterpret_cast<void*>(&GraphicsCommandListInterceptor::on_dispatch));

    GraphicsCommandListInterceptor::detach(reinterpret_cast<ID3D12GraphicsCommandList*>(&list));
    assert(list.vtable->draw == g_fake_list_vt.draw);
    assert(list.vtable->dispatch == g_fake_list_vt.dispatch);

    shutdown();
#else
    printf("  Skipped on non-Windows platform.\n");
#endif

    printf("Result: PASS\n");
    return 0;
}

}

int main() {
    return synapse::d3d12::main();
}
