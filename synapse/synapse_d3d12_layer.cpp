/**
 * @file synapse_d3d12_layer.cpp
 * @brief D3D12 COM vtable interception implementation.
 *
 * Provides:
 *  - vtable::patch() / vtable::restore() helpers
 *  - CommandQueueInterceptor::attach() / detach()
 *  - GraphicsCommandListInterceptor::attach() / detach()
 */

#include "synapse_d3d12_layer.h"

#if defined(_WIN32)
# include <windows.h>
# include <d3d12.h>
#endif

#include <cassert>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace synapse::d3d12 {

namespace {

#if defined(_WIN32)

std::once_flag g_init_once_flag;
HRESULT g_init_hr = S_OK;
bool g_enabled = false;
std::mutex g_global_mutex;

struct HookState {
    void* original = nullptr;
    void* patched  = nullptr;
};

std::unordered_map<ID3D12CommandQueue*, HookState>   g_queue_hooks;
std::unordered_map<ID3D12GraphicsCommandList*, HookState> g_list_hooks;
std::mutex g_hooks_map_mutex;

D3D12Hook g_queue_hook   = nullptr;
D3D12Hook g_list_hook    = nullptr;

// ID3D12CommandQueue vtable indices
constexpr size_t kQueue_ExecuteCommandLists = 10;

// ID3D12GraphicsCommandList vtable indices
constexpr size_t kList_DrawIndexedInstanced = 15;
constexpr size_t kList_DrawInstanced        = 16;
constexpr size_t kList_Dispatch             = 25;

void*& vtable_slot(ID3D12CommandQueue* queue, size_t index) {
    return *reinterpret_cast<void**>(*reinterpret_cast<void***>(queue) + index);
}

void*& vtable_slot(ID3D12GraphicsCommandList* list, size_t index) {
    return *reinterpret_cast<void**>(*reinterpret_cast<void***>(list) + index);
}

void* patch_impl(void** object_vtable, size_t index, void* replace) {
    if (!object_vtable || !replace) return nullptr;
    void* original = object_vtable[index];
    object_vtable[index] = replace;
    return original;
}

void restore_impl(void** object_vtable, size_t index, void* original) {
    if (!object_vtable || !original) return;
    object_vtable[index] = original;
}

/** @brief Trampoline: ExecuteCommandLists hook. */
void __cdecl trampoline_execute_command_lists(
    ID3D12CommandQueue* queue,
    unsigned int NumLists,
    ID3D12CommandList* const* ppLists) noexcept
{
    auto it = g_queue_hooks.find(queue);
    if (it != g_queue_hooks.end() && it->second.original) {
        using Fn = void(__cdecl*)(ID3D12CommandQueue*, unsigned int, ID3D12CommandList* const*);
        Fn original = reinterpret_cast<Fn>(it->second.original);
        original(queue, NumLists, ppLists);
    }

    if (g_queue_hook) {
        g_queue_hook(static_cast<void*>(queue),
                     static_cast<void*>(const_cast<ID3D12CommandList**>(ppLists)));
    }
}

/** @brief Trampoline: DrawIndexedInstanced hook. */
void __cdecl trampoline_draw_indexed_instanced(
    ID3D12GraphicsCommandList* list,
    unsigned int IndexCountPerInstance,
    unsigned int InstanceCount,
    unsigned int StartIndexLocation,
    INT BaseVertexLocation,
    unsigned int StartInstanceLocation) noexcept
{
    auto it = g_list_hooks.find(list);
    if (it != g_list_hooks.end() && it->second.original) {
        using Fn = void(__cdecl*)(ID3D12GraphicsCommandList*, unsigned int, unsigned int, unsigned int, INT, unsigned int);
        Fn original = reinterpret_cast<Fn>(it->second.original);
        original(list, IndexCountPerInstance, InstanceCount, StartIndexLocation,
                 BaseVertexLocation, StartInstanceLocation);
    }

    if (g_list_hook) {
        g_list_hook(static_cast<void*>(list), nullptr);
    }
}

/** @brief Trampoline: DrawInstanced hook. */
void __cdecl trampoline_draw_instanced(
    ID3D12GraphicsCommandList* list,
    unsigned int VertexCountPerInstance,
    unsigned int StartVertexLocation,
    unsigned int StartInstanceLocation) noexcept
{
    auto it = g_list_hooks.find(list);
    if (it != g_list_hooks.end() && it->second.original) {
        using Fn = void(__cdecl*)(ID3D12GraphicsCommandList*, unsigned int, unsigned int, unsigned int);
        Fn original = reinterpret_cast<Fn>(it->second.original);
        original(list, VertexCountPerInstance, StartVertexLocation, StartInstanceLocation);
    }

    if (g_list_hook) {
        g_list_hook(static_cast<void*>(list), nullptr);
    }
}

/** @brief Trampoline: Dispatch hook. */
void __cdecl trampoline_dispatch(
    ID3D12GraphicsCommandList* list,
    unsigned int ThreadGroupCountX,
    unsigned int ThreadGroupCountY,
    unsigned int ThreadGroupCountZ) noexcept
{
    auto it = g_list_hooks.find(list);
    if (it != g_list_hooks.end() && it->second.original) {
        using Fn = void(__cdecl*)(ID3D12GraphicsCommandList*, unsigned int, unsigned int, unsigned int);
        Fn original = reinterpret_cast<Fn>(it->second.original);
        original(list, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }

    if (g_list_hook) {
        g_list_hook(static_cast<void*>(list), nullptr);
    }
}

#else
// Stubs for non-Windows targets.

void*& vtable_slot(ID3D12CommandQueue*, size_t) { static void* dummy = nullptr; return dummy; }
void*& vtable_slot(ID3D12GraphicsCommandList*, size_t) { static void* dummy = nullptr; return dummy; }

#endif

} // anonymous namespace

// ---- Public API ----

HRESULT initialize() noexcept {
#if defined(_WIN32)
    std::call_once(g_init_once_flag, []() {
        g_init_hr = S_OK;
    });
    return g_init_hr;
#else
    return E_NOTIMPL;
#endif
}

void shutdown() noexcept {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(g_global_mutex);
    g_enabled = false;
    g_queue_hook = nullptr;
    g_list_hook  = nullptr;

    std::lock_guard<std::mutex> hooks_lock(g_hooks_map_mutex);
    g_queue_hooks.clear();
    g_list_hooks.clear();
#endif
}

HRESULT enable_d3d12_interception(bool enabled) noexcept {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (g_init_hr != S_OK) return E_FAIL;
    g_enabled = enabled;
    return S_OK;
#else
    return E_NOTIMPL;
#endif
}

bool is_d3d12_interception_enabled() noexcept {
#if defined(_WIN32)
    return g_enabled;
#else
    return false;
#endif
}

// ---- vtable namespace ----

void* vtable::patch(void** object_vtable, size_t index, void* replace) noexcept {
#if defined(_WIN32)
    return patch_impl(object_vtable, index, replace);
#else
    (void)object_vtable; (void)index; (void)replace;
    return nullptr;
#endif
}

void vtable::restore(void** object_vtable, size_t index, void* original) noexcept {
#if defined(_WIN32)
    restore_impl(object_vtable, index, original);
#else
    (void)object_vtable; (void)index; (void)original;
#endif
}

// ---- CommandQueueInterceptor ----

HRESULT CommandQueueInterceptor::attach(ID3D12CommandQueue* queue,
                                        D3D12Hook hook) noexcept {
#if defined(_WIN32)
    if (!queue || !hook || !is_d3d12_interception_enabled()) return E_INVALIDARG;

    std::lock_guard<std::mutex> lock(g_hooks_map_mutex);
    if (g_queue_hooks.find(queue) != g_queue_hooks.end()) return E_ABORT;

    void** vt = *reinterpret_cast<void***>(queue);
    HookState state;
    state.original = vt[kQueue_ExecuteCommandLists];
    state.patched  = reinterpret_cast<void*>(&trampoline_execute_command_lists);
    vt[kQueue_ExecuteCommandLists] = state.patched;
    g_queue_hooks[queue] = state;
    g_queue_hook = hook;
    return S_OK;
#else
    (void)queue; (void)hook;
    return E_NOTIMPL;
#endif
}

void CommandQueueInterceptor::detach(ID3D12CommandQueue* queue) noexcept {
#if defined(_WIN32)
    if (!queue) return;
    std::lock_guard<std::mutex> lock(g_hooks_map_mutex);
    auto it = g_queue_hooks.find(queue);
    if (it == g_queue_hooks.end()) return;

    void** vt = *reinterpret_cast<void***>(queue);
    vt[kQueue_ExecuteCommandLists] = it->second.original;
    g_queue_hooks.erase(it);
#endif
}

void CommandQueueInterceptor::on_execute_command_lists(
    ID3D12CommandQueue* /*queue*/,
    unsigned int /*NumLists*/,
    ID3D12CommandList* const* /*ppLists*/) noexcept
{
    // Hook callback logic lives in trampoline_execute_command_lists().
}

// ---- GraphicsCommandListInterceptor ----

HRESULT GraphicsCommandListInterceptor::attach(
    ID3D12GraphicsCommandList* list,
    D3D12Hook hook) noexcept
{
#if defined(_WIN32)
    if (!list || !hook || !is_d3d12_interception_enabled()) return E_INVALIDARG;

    std::lock_guard<std::mutex> lock(g_hooks_map_mutex);
    if (g_list_hooks.find(list) != g_list_hooks.end()) return E_ABORT;

    void** vt = *reinterpret_cast<void***>(list);
    HookState state;
    state.original = vt[kList_DrawIndexedInstanced];
    state.patched  = reinterpret_cast<void*>(&trampoline_draw_indexed_instanced);
    vt[kList_DrawIndexedInstanced] = state.patched;

    state.original = vt[kList_DrawInstanced];
    state.patched  = reinterpret_cast<void*>(&trampoline_draw_instanced);
    vt[kList_DrawInstanced] = state.patched;

    state.original = vt[kList_Dispatch];
    state.patched  = reinterpret_cast<void*>(&trampoline_dispatch);
    vt[kList_Dispatch] = state.patched;

    g_list_hooks[list] = state;
    g_list_hook = hook;
    return S_OK;
#else
    (void)list; (void)hook;
    return E_NOTIMPL;
#endif
}

void GraphicsCommandListInterceptor::detach(
    ID3D12GraphicsCommandList* list) noexcept
{
#if defined(_WIN32)
    if (!list) return;
    std::lock_guard<std::mutex> lock(g_hooks_map_mutex);
    auto it = g_list_hooks.find(list);
    if (it == g_list_hooks.end()) return;

    void** vt = *reinterpret_cast<void***>(list);
    // Restore all slots we may have patched.
    vt[kList_DrawIndexedInstanced] = it->second.original;
    vt[kList_DrawInstanced]        = it->second.original;
    vt[kList_Dispatch]             = it->second.original;
    g_list_hooks.erase(it);
#endif
}

void GraphicsCommandListInterceptor::on_draw_indexed_instanced(
    ID3D12GraphicsCommandList* /*list*/,
    unsigned int /*IndexCountPerInstance*/,
    unsigned int /*InstanceCount*/,
    unsigned int /*StartIndexLocation*/,
    INT /*BaseVertexLocation*/,
    unsigned int /*StartInstanceLocation*/) noexcept
{
    // Hook callback logic lives in trampoline_draw_indexed_instanced().
}

void GraphicsCommandListInterceptor::on_draw_instanced(
    ID3D12GraphicsCommandList* /*list*/,
    unsigned int /*VertexCountPerInstance*/,
    unsigned int /*InstanceCount*/,
    unsigned int /*StartVertexLocation*/,
    unsigned int /*StartInstanceLocation*/) noexcept
{
    // Hook callback logic lives in trampoline_draw_instanced().
}

void GraphicsCommandListInterceptor::on_dispatch(
    ID3D12GraphicsCommandList* /*list*/,
    unsigned int /*ThreadGroupCountX*/,
    unsigned int /*ThreadGroupCountY*/,
    unsigned int /*ThreadGroupCountZ*/) noexcept
{
    // Hook callback logic lives in trampoline_dispatch().
}

} // namespace synapse::d3d12
