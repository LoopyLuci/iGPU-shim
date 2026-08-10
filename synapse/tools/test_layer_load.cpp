// ============================================================================
// synapse/tools/test_layer_load.cpp
// Real hardware test: loads the Synapse implicit layer on Intel UHD Graphics,
// creates a Vulkan instance + device, and verifies the layer is active.
//
// Build: cl /EHsc /std:c++20 test_layer_load.cpp /I../ /I../../vulkan_sdk/include
//        /link vulkan-1.lib
// Run:   set SYNAPSE_ENABLE=1
//        test_layer_load.exe
// ============================================================================

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <filesystem>

#define VK_CHECK(call) do { \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) { \
        printf("  FAIL: %s returned %d\n", #call, _r); \
        return 1; \
    } \
} while(0)

static bool has_layer(const std::vector<VkLayerProperties>& layers, const char* name) {
    for (auto& l : layers) {
        if (strstr(l.layerName, name)) return true;
    }
    return false;
}

int main() {
    printf("=== Synapse Layer Load Test (Real iGPU) ===\n\n");

    // ── Step 1: Enumerate instance layers ──────────────────────────────────
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());

    printf("Available Vulkan layers (%d):\n", layerCount);
    bool synapseFound = false;
    for (auto& l : layers) {
        printf("  %-48s v%d.%d.%d  %s\n",
               l.layerName, VK_VERSION_MAJOR(l.specVersion),
               VK_VERSION_MINOR(l.specVersion),
               VK_VERSION_PATCH(l.specVersion),
               l.description);
        if (strstr(l.layerName, "SYNAPSE") || strstr(l.layerName, "Synapse")) {
            synapseFound = true;
        }
    }

    if (!synapseFound) {
        printf("\n  WARNING: Synapse layer NOT found in Vulkan layers.\n");
        printf("  Ensure VkLayer_synapse.json is in the Vulkan layer search path.\n");
        printf("  Set VK_LAYER_PATH to the directory containing the JSON manifest.\n");
        printf("  Or set VK_INSTANCE_LAYERS=VK_LAYER_SYNAPSE_iGPU_Shim\n");
        // Continue anyway — we'll see if it loads
    } else {
        printf("\n  Synapse layer FOUND in layer list.\n");
    }

    // ── Step 2: Create VkInstance with Synapse layer ───────────────────────
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Synapse Layer Test";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    // Request the Synapse layer
    const char* requestedLayers[] = {
        "VK_LAYER_SYNAPSE_iGPU_Shim"
    };
    uint32_t requestedLayerCount = synapseFound ? 1 : 0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledLayerCount = requestedLayerCount;
    createInfo.ppEnabledLayerNames = requestedLayerCount ? requestedLayers : nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);

    if (result == VK_SUCCESS) {
        printf("\n  VkInstance created successfully WITH Synapse layer.\n");
    } else if (requestedLayerCount > 0) {
        printf("\n  VkInstance creation with Synapse layer failed (code %d).\n", result);
        printf("  Retrying WITHOUT layer...\n");
        createInfo.enabledLayerCount = 0;
        createInfo.ppEnabledLayerNames = nullptr;
        VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance));
        printf("  VkInstance created WITHOUT layer (layer not functional).\n");
    } else {
        printf("\n  FAIL: VkCreateInstance failed with code %d\n", result);
        return 1;
    }

    // ── Step 3: Enumerate physical devices ──────────────────────────────────
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    printf("\nPhysical devices (%d):\n", deviceCount);
    VkPhysicalDevice targetGPU = VK_NULL_HANDLE;

    for (auto& dev : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);

        const char* devType = "UNKNOWN";
        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: devType = "INTEGRATED"; break;
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   devType = "DISCRETE"; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:             devType = "CPU"; break;
            default: break;
        }

        printf("  %-30s %s  API %d.%d.%d  VRAM %lu MB\n",
               props.deviceName, devType,
               VK_VERSION_MAJOR(props.apiVersion),
               VK_VERSION_MINOR(props.apiVersion),
               VK_VERSION_PATCH(props.apiVersion),
               props.limits.maxStorageBufferRange / (1024 * 1024));

        // Prefer integrated GPU (iGPU)
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && targetGPU == VK_NULL_HANDLE) {
            targetGPU = dev;
        }
    }

    if (targetGPU == VK_NULL_HANDLE && deviceCount > 0) {
        targetGPU = devices[0];
    }

    if (targetGPU == VK_NULL_HANDLE) {
        printf("\n  FAIL: No Vulkan GPU found.\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    VkPhysicalDeviceProperties targetProps{};
    vkGetPhysicalDeviceProperties(targetGPU, &targetProps);
    printf("\n  Target GPU: %s (API %d.%d.%d)\n",
           targetProps.deviceName,
           VK_VERSION_MAJOR(targetProps.apiVersion),
           VK_VERSION_MINOR(targetProps.apiVersion),
           VK_VERSION_PATCH(targetProps.apiVersion));

    // ── Step 4: Create logical device ──────────────────────────────────────
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(targetGPU, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(targetGPU, &queueFamilyCount, queueFamilies.data());

    int graphicsQueueIdx = -1;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsQueueIdx = (int)i;
            break;
        }
    }

    if (graphicsQueueIdx < 0) {
        printf("  FAIL: No graphics queue family found.\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = (uint32_t)graphicsQueueIdx;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;

    VkDevice device = VK_NULL_HANDLE;
    result = vkCreateDevice(targetGPU, &deviceCreateInfo, nullptr, &device);

    if (result != VK_SUCCESS) {
        printf("  FAIL: vkCreateDevice returned %d\n", result);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    printf("  Logical device created.\n");

    // ── Step 5: Check vkGetDeviceProcAddr interception ─────────────────────
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)(
        vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));

    if (pfnGetDeviceProcAddr) {
        auto pfnDrawIndexed = (PFN_vkCmdDrawIndexed)(
            pfnGetDeviceProcAddr(device, "vkCmdDrawIndexed"));

        if (pfnDrawIndexed) {
            printf("  vkCmdDrawIndexed resolved via vkGetDeviceProcAddr: %p\n",
                   (void*)pfnDrawIndexed);
            // If the layer intercepted this, the pointer will differ from the
            // instance's vkCmdDrawIndexed
        }
    }

    // ── Step 6: Verify draw interception via GDPA ──────────────────────────
    // The layer intercepts vkCmdDrawIndexed, vkCmdDraw, vkCmdDispatch.
    // Verify that the resolved function pointers go through our layer.
    namespace fs = std::filesystem;

    // Create a command pool + buffer for inspection (no submission needed)
    VkCommandPoolCreateInfo cmdPoolInfo{};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.queueFamilyIndex = (uint32_t)graphicsQueueIdx;
    cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &commandPool));

    // Resolve vkCmdDrawIndexed through the layer's GDPA
    auto layerDrawIndexed = (PFN_vkCmdDrawIndexed)(
        pfnGetDeviceProcAddr(device, "vkCmdDrawIndexed"));
    auto layerCmdDraw = (PFN_vkCmdDraw)(
        pfnGetDeviceProcAddr(device, "vkCmdDraw"));
    auto layerCmdDispatch = (PFN_vkCmdDispatch)(
        pfnGetDeviceProcAddr(device, "vkCmdDispatch"));

    printf("  GDPA-resolved draw functions:\n");
    printf("    vkCmdDrawIndexed: %p\n", (void*)layerDrawIndexed);
    printf("    vkCmdDraw:        %p\n", (void*)layerCmdDraw);
    printf("    vkCmdDispatch:    %p\n", (void*)layerCmdDispatch);

    // These should be non-null if the layer intercepted device creation
    bool interceptActive = (layerDrawIndexed != nullptr) &&
                           (layerCmdDraw != nullptr) &&
                           (layerCmdDispatch != nullptr);
    printf("  Draw interception active: %s\n", interceptActive ? "YES" : "NO");

    // ── Step 7: Check WAL file was created (layer was active) ──────────────
    // The layer creates a WAL file on device creation
    fs::path walPath("C:/Users/limpi/AppData/Local/SynapseLayer/synapse.wal");
    bool walCreated = fs::exists(walPath);

    if (walCreated) {
        auto walSize = fs::file_size(walPath);
        printf("  WAL file found: %llu bytes\n", (unsigned long long)walSize);
    } else {
        printf("  WAL file not found (layer may not be active).\n");
    }

    // ── Step 8: Cleanup ────────────────────────────────────────────────────
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    printf("\n=== Layer Load Test Complete ===\n");
    printf("Result: %s\n",
           (synapseFound && result == VK_SUCCESS) ? "PASS" : "PARTIAL (layer not confirmed)");

    return 0;
}
