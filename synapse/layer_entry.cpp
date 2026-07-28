// ============================================================================\
// synapse/layer_entry.cpp
// Project Synapse – Vulkan Implicit Layer Entry Points
//
// Exports the two symbols declared in VkLayer_synapse.json:
//   SynapseLayer_vkGetInstanceProcAddr
//   SynapseLayer_vkGetDeviceProcAddr
//
// Hooks vkCmdDrawIndexed, vkCmdDraw, vkCmdDispatch, vkCmdPushConstants,
// vkCmdBindDescriptorSets, vkCmdBindPipeline, vkCreateImage, vkDestroyImage
// and vkCmdBindShadersEXT into SynapseCore so every GPU-accessing Vulkan
// command passes through the ML-guided backend selector.
//
// See VkLayer_synapse.json.in for the loader manifest.
// ============================================================================\
#include "synapse_core.h"

#include <vulkan/vk_layer.h>   // VkLayerInstanceCreateInfo, VK_LAYER_LINK_INFO
#include <vulkan/vulkan.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

// ── Dispatch-key utilities ────────────────────────────────────────────────────
namespace {

using DispatchKey = void*;

inline DispatchKey dispatch_key(const void* dispatchable_handle) noexcept {
    return *static_cast<void* const*>(dispatchable_handle);
}

// ── Per-instance state ──────────────────────────────────────────────────
struct InstanceCtx {
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
};
std::unordered_map<DispatchKey, InstanceCtx> g_instances;
std::mutex g_instance_mutex;

// ── Per-device state ──────────────────────────────────────────────────
struct DeviceCtx {
    PFN_vkGetDeviceProcAddr  next_gdpa                = nullptr;
    PFN_vkCmdDrawIndexed      orig_draw_indexed        = nullptr;
    PFN_vkCmdDraw             orig_draw                = nullptr;
    PFN_vkCmdDispatch         orig_dispatch              = nullptr;
    PFN_vkCmdPushConstants    orig_push_constants      = nullptr;
    PFN_vkCmdBindDescriptorSets orig_bind_descriptor_sets = nullptr;
    PFN_vkCmdBindPipeline     orig_bind_pipeline         = nullptr;
    PFN_vkCmdBindShadersEXT   orig_bind_shaders          = nullptr;
    PFN_vkFreeCommandBuffers  orig_free_cmd_bufs        = nullptr;
    PFN_vkCreateImage         orig_create_image         = nullptr;
    PFN_vkDestroyImage        orig_destroy_image        = nullptr;
    std::unique_ptr<synapse::SynapseCore> core;
};
std::unordered_map<DispatchKey, DeviceCtx> g_devices;
std::mutex g_device_mutex;

// ── Chain-link helpers ────────────────────────────────────────────────
static VkLayerInstanceCreateInfo* find_instance_link(
        const VkInstanceCreateInfo* pCI) noexcept {
    const auto* p = static_cast<const VkBaseInStructure*>(
        static_cast<const void*>(pCI->pNext));
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            auto* lci = const_cast<VkLayerInstanceCreateInfo*>(
                reinterpret_cast<const VkLayerInstanceCreateInfo*>(p));
            if (lci->function == VK_LAYER_LINK_INFO)
                return lci;
        }
        p = p->pNext;
    }
    return nullptr;
}

static VkLayerDeviceCreateInfo* find_device_link(
        const VkDeviceCreateInfo* pCI) noexcept {
    const auto* p = static_cast<const VkBaseInStructure*>(
        static_cast<const void*>(pCI->pNext));
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            auto* lci = const_cast<VkLayerDeviceCreateInfo*>(
                reinterpret_cast<const VkLayerDeviceCreateInfo*>(p));
            if (lci->function == VK_LAYER_LINK_INFO)
                return lci;
        }
        p = p->pNext;
    }
    return nullptr;
}

} // anonymous namespace

// ── vkCreateInstance ──────────────────────────────────────────────────────────
VKAPI_ATTR VkResult VKAPI_CALL SynapseLayer_vkCreateInstance(
    const VkInstanceCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance*                  pInstance)
{
    VkLayerInstanceCreateInfo* link = find_instance_link(pCreateInfo);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    // Consume this layer's link entry.
    PFN_vkGetInstanceProcAddr next_gipa =
        link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto* fn = reinterpret_cast<PFN_vkCreateInstance>(
        next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = fn(pCreateInfo, pAllocator, pInstance);
    if (r != VK_SUCCESS) return r;

    std::lock_guard<std::mutex> lock(g_instance_mutex);
    g_instances[dispatch_key(*pInstance)].next_gipa = next_gipa;
    return VK_SUCCESS;
}

// ── vkDestroyInstance ─────────────────────────────────────────────────────────
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkDestroyInstance(
    VkInstance                   instance,
    const VkAllocationCallbacks* pAllocator)
{
    DispatchKey key = dispatch_key(instance);
    PFN_vkDestroyInstance orig = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_instance_mutex);
        auto it = g_instances.find(key);
        if (it != g_instances.end()) {
            orig = reinterpret_cast<PFN_vkDestroyInstance>(
                it->second.next_gipa(instance, "vkDestroyInstance"));
            g_instances.erase(it);
        }
    }
    if (orig) orig(instance, pAllocator);
}

// ── vkCreateDevice ────────────────────────────────────────────────────────────
VKAPI_ATTR VkResult VKAPI_CALL SynapseLayer_vkCreateDevice(
    VkPhysicalDevice             physicalDevice,
    const VkDeviceCreateInfo*    pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice*                    pDevice)
{
    VkLayerDeviceCreateInfo* link = find_device_link(pCreateInfo);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa =
        link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa =
        link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto* create_fn = reinterpret_cast<PFN_vkCreateDevice>(
        next_gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!create_fn) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = create_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (r != VK_SUCCESS) return r;

    auto orig_draw = reinterpret_cast<PFN_vkCmdDrawIndexed>(
        next_gdpa(*pDevice, "vkCmdDrawIndexed"));
    auto orig_draw_non_indexed = reinterpret_cast<PFN_vkCmdDraw>(
        next_gdpa(*pDevice, "vkCmdDraw"));
    auto orig_dispatch = reinterpret_cast<PFN_vkCmdDispatch>(
        next_gdpa(*pDevice, "vkCmdDispatch"));
    auto orig_push_constants = reinterpret_cast<PFN_vkCmdPushConstants>(
        next_gdpa(*pDevice, "vkCmdPushConstants"));
    auto orig_bind_desc_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
        next_gdpa(*pDevice, "vkCmdBindDescriptorSets"));
    auto orig_bind_pipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(
        next_gdpa(*pDevice, "vkCmdBindPipeline"));
    auto orig_bind_shaders = reinterpret_cast<PFN_vkCmdBindShadersEXT>(
        next_gdpa(*pDevice, "vkCmdBindShadersEXT"));
    auto orig_free_cmd_bufs = reinterpret_cast<PFN_vkFreeCommandBuffers>(
        next_gdpa(*pDevice, "vkFreeCommandBuffers"));
    auto orig_create_image = reinterpret_cast<PFN_vkCreateImage>(
        next_gdpa(*pDevice, "vkCreateImage"));
    auto orig_destroy_image = reinterpret_cast<PFN_vkDestroyImage>(
        next_gdpa(*pDevice, "vkDestroyImage"));

    std::lock_guard<std::mutex> lock(g_device_mutex);
    DeviceCtx& ctx = g_devices[dispatch_key(*pDevice)];
    ctx.next_gdpa              = next_gdpa;
    ctx.orig_draw_indexed      = orig_draw;
    ctx.orig_draw              = orig_draw_non_indexed;
    ctx.orig_dispatch            = orig_dispatch;
    ctx.orig_push_constants    = orig_push_constants;
    ctx.orig_bind_descriptor_sets = orig_bind_desc_sets;
    ctx.orig_bind_pipeline     = orig_bind_pipeline;
    ctx.orig_bind_shaders      = orig_bind_shaders;
    ctx.orig_free_cmd_bufs     = orig_free_cmd_bufs;
    ctx.orig_create_image      = orig_create_image;
    ctx.orig_destroy_image     = orig_destroy_image;
    // SynapseCore owns the ML pipeline and hooks the draw path.
    ctx.core = std::make_unique<synapse::SynapseCore>(
        orig_draw, orig_draw_non_indexed, orig_dispatch,
        orig_push_constants, orig_bind_desc_sets, orig_bind_shaders);
    // Wire the ITS engine's power estimator so bandwidth accounting works.
    ctx.core->wire_power_estimator(&ctx.core->power_estimator());
    return VK_SUCCESS;
}

// ── vkDestroyDevice ───────────────────────────────────────────────────────────
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkDestroyDevice(
    VkDevice                     device,
    const VkAllocationCallbacks* pAllocator)
{
    DispatchKey key = dispatch_key(device);
    PFN_vkDestroyDevice orig = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            orig = reinterpret_cast<PFN_vkDestroyDevice>(
                it->second.next_gdpa(device, "vkDestroyDevice"));
            g_devices.erase(it);
        }
    }
    if (orig) orig(device, pAllocator);
}

// ── hooked vkCmdDrawIndexed ───────────────────────────────────────────────────
// This is the hot path. The dispatch-key lookup is O(1) (unordered_map).
// We take a shared read on g_devices only; SynapseCore is thread-safe internally.
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkCmdDrawIndexed(
    VkCommandBuffer commandBuffer,
    uint32_t        indexCount,
    uint32_t        instanceCount,
    uint32_t        firstIndex,
    int32_t         vertexOffset,
    uint32_t        firstInstance)
{
    DispatchKey key = dispatch_key(commandBuffer);

    synapse::SynapseCore* core = nullptr;
    PFN_vkCmdDrawIndexed  fallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            core     = it->second.core.get();
            fallback = it->second.orig_draw_indexed;
        }
    }

    if (core) {
        // Forward to ML-guided backend selector.
        // All parameters are forwarded as-is; SynapseCore
        // captures its own WorkloadSignature.
        core->handle_draw_indexed(
            commandBuffer, indexCount, instanceCount,
            firstIndex, vertexOffset, firstInstance);
    } else if (fallback) {
        fallback(commandBuffer, indexCount, instanceCount,
                 firstIndex, vertexOffset, firstInstance);
    }
    // If both are null the draw call is a no-op, which is safe.
}

// ── hooked vkCmdDraw (non-indexed) ──────────────────────────────────
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkCmdDraw(
    VkCommandBuffer commandBuffer,
    uint32_t        vertexCount,
    uint32_t        instanceCount,
    uint32_t        firstVertex,
    uint32_t        firstInstance)
{
    DispatchKey key = dispatch_key(commandBuffer);
    synapse::SynapseCore* core = nullptr;
    PFN_vkCmdDraw fallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            core    = it->second.core.get();
            fallback = it->second.orig_draw;
        }
    }
    if (core) {
        core->handle_draw(commandBuffer, vertexCount, instanceCount,
                            firstVertex, firstInstance);
    } else if (fallback) {
        fallback(commandBuffer, vertexCount, instanceCount,
                 firstVertex, firstInstance);
    }
}

// ── hooked vkCmdDispatch ────────────────────────────────────────────
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkCmdDispatch(
    VkCommandBuffer commandBuffer,
    uint32_t        groupCountX,
    uint32_t        groupCountY,
    uint32_t        groupCountZ)
{
    DispatchKey key = dispatch_key(commandBuffer);
    synapse::SynapseCore* core = nullptr;
    PFN_vkCmdDispatch fallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            core     = it->second.core.get();
            fallback = it->second.orig_dispatch;
        }
    }
    if (core) {
        core->handle_dispatch(commandBuffer, groupCountX, groupCountY,
                                groupCountZ);
    } else if (fallback) {
        fallback(commandBuffer, groupCountX, groupCountY, groupCountZ);
    }
}

// ── hooked vkCmdPushConstants ───────────────────────────────────────
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkCmdPushConstants(
    VkCommandBuffer commandBuffer,
    VkPipelineLayout layout,
    uint32_t        offset,
    uint32_t        size,
    const void*     pValues)
{
    DispatchKey key = dispatch_key(commandBuffer);
    synapse::SynapseCore* core = nullptr;
    PFN_vkCmdPushConstants fallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            core     = it->second.core.get();
            fallback = it->second.orig_push_constants;
        }
    }
    if (core) {
        core->handle_push_constants(commandBuffer, layout, offset, size, pValues);
    }
    if (fallback) {
        fallback(commandBuffer, layout, offset, size, pValues);
    }
}

// ── hooked vkCmdBindDescriptorSets ──────────────────────────────────
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkCmdBindDescriptorSets(
    VkCommandBuffer                    commandBuffer,
    VkPipelineBindPoint                bindPoint,
    VkPipelineLayout                   layout,
    uint32_t                           firstSet,
    uint32_t                           descriptorSetCount,
    const VkDescriptorSet*             pDescriptorSets,
    uint32_t                           dynamicOffsetCount,
    const uint32_t*                    pDynamicOffsets)
{
    DispatchKey key = dispatch_key(commandBuffer);
    synapse::SynapseCore* core = nullptr;
    PFN_vkCmdBindDescriptorSets fallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            core     = it->second.core.get();
            fallback = it->second.orig_bind_descriptor_sets;
        }
    }
    if (core) {
        core->handle_bind_descriptor_sets(
            commandBuffer, bindPoint, layout, firstSet,
            descriptorSetCount, pDescriptorSets,
            dynamicOffsetCount, pDynamicOffsets);
    }
    if (fallback) {
        fallback(commandBuffer, bindPoint, layout, firstSet,
                     descriptorSetCount, pDescriptorSets,
                     dynamicOffsetCount, pDynamicOffsets);
    }
}

// ── hooked vkCmdBindShadersEXT (VK_EXT_shader_object) ────────────────
// Records the bound shader stages for JIT specialization tracking.
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkCmdBindShadersEXT(
    VkCommandBuffer commandBuffer,
    uint32_t        stageCount,
    const VkShaderStageFlagBits* pStages,
    const VkShaderEXT*        pShaders)
{
    // No per-shader tracking needed here — the pipeline bind
    // (vkCmdBindPipeline) already captures the shader hash.
    DispatchKey key = dispatch_key(commandBuffer);
    PFN_vkCmdBindShadersEXT fallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            fallback = it->second.orig_bind_shaders;
        }
    }
    if (fallback) {
        fallback(commandBuffer, stageCount, pStages, pShaders);
    }
}

// ── hooked vkCreateImage ────────────────────────────────────────────────
// Registers the image with the ITS engine so mip residency can be tracked.
VKAPI_ATTR VkResult VKAPI_CALL SynapseLayer_vkCreateImage(
    VkDevice                       device,
    const VkImageCreateInfo*       pCreateInfo,
    const VkAllocationCallbacks*   pAllocator,
    VkImage*                       pImage)
{
    DispatchKey key = dispatch_key(device);
    PFN_vkCreateImage fallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            fallback = it->second.orig_create_image;
        }
    }
    VkResult r = fallback
        ? fallback(device, pCreateInfo, pAllocator, pImage)
        : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && pImage && *pImage != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end() && it->second.core) {
            it->second.core->notify_create_image(
                *pImage, pCreateInfo,
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(*pImage)));
        }
    }
    return r;
}

// ── hooked vkDestroyImage ────────────────────────────────────────────
// Unregisters the image from the ITS engine.
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkDestroyImage(
    VkDevice                       device,
    VkImage                        image,
    const VkAllocationCallbacks*   pAllocator)
{
    DispatchKey key = dispatch_key(device);
    PFN_vkDestroyImage fallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            fallback = it->second.orig_destroy_image;
            if (it->second.core) {
                it->second.core->notify_destroy_image(image);
            }
        }
    }
    if (fallback) {
        fallback(device, image, pAllocator);
    }
}

// ── hooked vkCmdBindPipeline ──────────────────────────────────────────────────
// Records the bound VkPipeline handle (cast to uint64_t) as shader_hash in the
// per-cmd-buf state map so capture_current_signature() gets a live hash value.
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkCmdBindPipeline(
    VkCommandBuffer     commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline          pipeline)
{
    DispatchKey key = dispatch_key(commandBuffer);
    synapse::SynapseCore* core     = nullptr;
    PFN_vkCmdBindPipeline fallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(key);
        if (it != g_devices.end()) {
            core     = it->second.core.get();
            fallback = it->second.orig_bind_pipeline;
        }
    }
    if (core)
        // The VkPipeline handle is unique per compiled pipeline object — a stable
        // opaque hash that distinguishes every distinct shader configuration.
        core->notify_bind_pipeline(
            commandBuffer, pipeline,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pipeline)));
    if (fallback)
        fallback(commandBuffer, pipelineBindPoint, pipeline);
}

// ── hooked vkFreeCommandBuffers ───────────────────────────────────────────────
// Purges per-cmd-buf state to prevent unbounded map growth during long sessions.
VKAPI_ATTR void VKAPI_CALL SynapseLayer_vkFreeCommandBuffers(
    VkDevice              device,
    VkCommandPool         commandPool,
    uint32_t              commandBufferCount,
    const VkCommandBuffer* pCommandBuffers)
{
    DispatchKey dkey = dispatch_key(device);
    synapse::SynapseCore*    core     = nullptr;
    PFN_vkFreeCommandBuffers fallback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_device_mutex);
        auto it = g_devices.find(dkey);
        if (it != g_devices.end()) {
            core     = it->second.core.get();
            fallback = it->second.orig_free_cmd_bufs;
        }
    }
    if (core) {
        for (uint32_t i = 0; i < commandBufferCount; ++i)
            if (pCommandBuffers[i] != VK_NULL_HANDLE)
                core->notify_free_cmd_buf(pCommandBuffers[i]);
    }
    if (fallback)
        fallback(device, commandPool, commandBufferCount, pCommandBuffers);
}

// ── vkGetDeviceProcAddr ───────────────────────────────────────────────────────
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL SynapseLayer_vkGetDeviceProcAddr(
    VkDevice    device,
    const char* pName)
{
    // Intercept the functions we hook.
    if (!strcmp(pName, "vkGetDeviceProcAddr"))   return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkGetDeviceProcAddr);
    if (!strcmp(pName, "vkDestroyDevice"))       return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkDestroyDevice);
    if (!strcmp(pName, "vkCmdDrawIndexed"))      return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCmdDrawIndexed);
    if (!strcmp(pName, "vkCmdBindPipeline"))     return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCmdBindPipeline);
    if (!strcmp(pName, "vkFreeCommandBuffers"))  return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkFreeCommandBuffers);

    // Pass everything else to the next layer / ICD.
    std::lock_guard<std::mutex> lock(g_device_mutex);
    auto it = g_devices.find(dispatch_key(device));
    if (it != g_devices.end() && it->second.next_gdpa)
        return it->second.next_gdpa(device, pName);
    return nullptr;
}

// ── vkGetInstanceProcAddr ─────────────────────────────────────────────────────
// This is the entry point named in the JSON manifest.  It must be exported
// with C linkage so the Vulkan loader can find it by name.
extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL SynapseLayer_vkGetInstanceProcAddr(
    VkInstance  instance,
    const char* pName)
{
    // Instance-level hooks
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkGetInstanceProcAddr);
    if (!strcmp(pName, "vkCreateInstance"))      return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCreateInstance);
    if (!strcmp(pName, "vkDestroyInstance"))     return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkDestroyInstance);

    // Device-level hooks surfaced via GIPA so loader can build the dispatch table.
    if (!strcmp(pName, "vkGetDeviceProcAddr"))   return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkGetDeviceProcAddr);
    if (!strcmp(pName, "vkCreateDevice"))        return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCreateDevice);
    if (!strcmp(pName, "vkDestroyDevice"))       return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkDestroyDevice);
    if (!strcmp(pName, "vkCmdDrawIndexed"))      return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCmdDrawIndexed);
    if (!strcmp(pName, "vkCmdDraw"))              return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCmdDraw);
    if (!strcmp(pName, "vkCmdDispatch"))          return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCmdDispatch);
    if (!strcmp(pName, "vkCmdPushConstants"))      return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCmdPushConstants);
    if (!strcmp(pName, "vkCmdBindDescriptorSets")) return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCmdBindDescriptorSets);
    if (!strcmp(pName, "vkCmdBindPipeline"))     return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCmdBindPipeline);
    if (!strcmp(pName, "vkCmdBindShadersEXT"))    return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCmdBindShadersEXT);
    if (!strcmp(pName, "vkFreeCommandBuffers"))  return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkFreeCommandBuffers);
    if (!strcmp(pName, "vkCreateImage"))         return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkCreateImage);
    if (!strcmp(pName, "vkDestroyImage"))        return reinterpret_cast<PFN_vkVoidFunction>(SynapseLayer_vkDestroyImage);

    // Chain to next layer / ICD for every unhooked function.
    if (instance == VK_NULL_HANDLE) return nullptr;
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    auto it = g_instances.find(dispatch_key(instance));
    if (it != g_instances.end() && it->second.next_gipa)
        return it->second.next_gipa(instance, pName);
    return nullptr;
}

} // extern "C"
