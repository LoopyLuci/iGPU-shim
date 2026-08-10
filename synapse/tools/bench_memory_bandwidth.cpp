// ============================================================================
// synapse/tools/bench_memory_bandwidth.cpp
// Real hardware benchmark: measures memory bandwidth via Vulkan buffer copies.
//
// Creates staging + device-local buffers, times vkCmdCopyBuffer operations,
// and reports throughput in MB/s.
//
// Run: VK_LAYER_PATH=<dir> VK_INSTANCE_LAYERS=VK_LAYER_SYNAPSE_iGPU_Shim
//      bench_memory_bandwidth.exe
// ============================================================================

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <string>
#include <algorithm>

#define VK_CHECK(call) do { \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) { \
        printf("  FAIL: %s returned %d\n", #call, _r); \
        return 1; \
    } \
} while(0)

static const size_t BUFFER_SIZE = 256 * 1024 * 1024;  // 256 MB
static const int ITERATIONS = 10;

int main() {
    printf("=== Memory Bandwidth Benchmark (Real iGPU) ===\n\n");
    printf("  Buffer size: %zu MB\n", BUFFER_SIZE / (1024 * 1024));
    printf("  Iterations: %d\n\n", ITERATIONS);

    // ── Step 1: Create instance + device ────────────────────────────────
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Synapse Memory Bandwidth";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    const char* layers[] = {"VK_LAYER_SYNAPSE_iGPU_Shim"};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;

    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> physDevs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, physDevs.data());

    VkPhysicalDevice physDev = physDevs[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physDev, &props);
    printf("  GPU: %s\n", props.deviceName);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDev, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDev, &qfCount, qfs.data());
    int gfxQ = -1;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfxQ = (int)i; break; }
    }
    if (gfxQ < 0) { printf("  FAIL: no graphics queue\n"); return 1; }

    float pri = 1.0f;
    VkDeviceQueueCreateInfo dqi{};
    dqi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqi.queueFamilyIndex = (uint32_t)gfxQ;
    dqi.queueCount = 1;
    dqi.pQueuePriorities = &pri;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqi;

    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(physDev, &dci, nullptr, &device));

    // ── Step 2: Find a memory type that is BOTH host-visible and device-local ──
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    uint32_t hostVisibleIdx = UINT32_MAX;
    uint32_t deviceLocalIdx = UINT32_MAX;

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags = memProps.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            hostVisibleIdx = i;
        }
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            deviceLocalIdx = i;
        }
    }

    printf("  Host-visible mem type: %u\n", hostVisibleIdx);
    printf("  Device-local mem type: %u\n", deviceLocalIdx);

    // ── Step 3: Create staging buffer (host-visible) ────────────────────
    VkBufferCreateInfo stagCI{};
    stagCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagCI.size = BUFFER_SIZE;
    stagCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(device, &stagCI, nullptr, &stagingBuf));

    VkMemoryRequirements stagReq{};
    vkGetBufferMemoryRequirements(device, stagingBuf, &stagReq);

    VkMemoryAllocateInfo stagAI{};
    stagAI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagAI.allocationSize = stagReq.size;
    stagAI.memoryTypeIndex = hostVisibleIdx;

    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(device, &stagAI, nullptr, &stagingMem));
    VK_CHECK(vkBindBufferMemory(device, stagingBuf, stagingMem, 0));

    // Fill staging buffer with pattern
    {
        void* p = nullptr;
        vkMapMemory(device, stagingMem, 0, BUFFER_SIZE, 0, &p);
        if (p) {
            memset(p, 0xAB, BUFFER_SIZE);
            vkUnmapMemory(device, stagingMem);
        }
    }

    // ── Step 4: Create device-local buffer ──────────────────────────────
    VkBufferCreateInfo devCI{};
    devCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    devCI.size = BUFFER_SIZE;
    devCI.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    devCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer deviceBuf = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(device, &devCI, nullptr, &deviceBuf));

    VkMemoryRequirements devReq{};
    vkGetBufferMemoryRequirements(device, deviceBuf, &devReq);

    VkMemoryAllocateInfo devAI{};
    devAI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    devAI.allocationSize = devReq.size;
    devAI.memoryTypeIndex = deviceLocalIdx;

    VkDeviceMemory deviceMem = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(device, &devAI, nullptr, &deviceMem));
    VK_CHECK(vkBindBufferMemory(device, deviceBuf, deviceMem, 0));

    // ── Step 5: Create command pool + buffer ────────────────────────────
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = (uint32_t)gfxQ;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &cpci, nullptr, &cmdPool));

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));

    // ── Step 6: Benchmark buffer copy ───────────────────────────────────
    VkBufferCopy region{};
    region.size = BUFFER_SIZE;

    std::vector<double> times;
    double totalTime = 0.0;

    printf("  Running %d copy iterations...\n", ITERATIONS);

    for (int i = 0; i < ITERATIONS; i++) {
        // Record command buffer
        VkCommandBufferBeginInfo begCI{};
        begCI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begCI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begCI));
        vkCmdCopyBuffer(cmd, stagingBuf, deviceBuf, 1, &region);
        VK_CHECK(vkEndCommandBuffer(cmd));

        // Submit and time
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, (uint32_t)gfxQ, 0, &queue);

        auto start = std::chrono::high_resolution_clock::now();

        VkSubmitInfo sub{};
        sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        sub.commandBufferCount = 1;
        sub.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1, &sub, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(queue));

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        times.push_back(ms);
        totalTime += ms;

        printf("    Iter %2d: %6.2f ms\n", i + 1, ms);
    }

    // ── Step 7: Calculate results ───────────────────────────────────────
    double avgMs = totalTime / ITERATIONS;
    double avgSec = avgMs / 1000.0;
    double mbPerSec = (BUFFER_SIZE / (1024.0 * 1024.0)) / avgSec;

    printf("\n  Results:\n");
    printf("    Average copy time:  %.2f ms\n", avgMs);
    printf("    Throughput:         %.1f MB/s\n", mbPerSec);
    printf("    Bandwidth:          %.2f Gb/s\n", mbPerSec * 8.0 / 1000.0);

    // ── Step 8: Cleanup ─────────────────────────────────────────────────
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyBuffer(device, deviceBuf, nullptr);
    vkFreeMemory(device, deviceMem, nullptr);
    vkDestroyBuffer(device, stagingBuf, nullptr);
    vkFreeMemory(device, stagingMem, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    printf("\n=== Memory Bandwidth Benchmark Complete ===\n");
    return 0;
}
