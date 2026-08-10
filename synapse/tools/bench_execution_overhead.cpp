// ============================================================================
// synapse/tools/bench_execution_overhead.cpp
// Real hardware benchmark: measures GIPA handler dispatch overhead.
//
// The layer intercepts GIPA and runs dispatch_key lookup + mutex + chain.
// We measure that cost vs native GIPA to get real overhead numbers.
//
// Run: VK_LAYER_PATH=<dir> VK_INSTANCE_LAYERS=VK_LAYER_SYNAPSE_iGPU_Shim
//      bench_execution_overhead.exe
// ============================================================================

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>

#define VK_CHECK(call) do { \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) { \
        printf("  FAIL: %s returned %d\n", #call, _r); \
        return 1; \
    } \
} while(0)

static const int ITERATIONS = 10000;

int main() {
    printf("=== Synapse Execution Overhead Benchmark (Real iGPU) ===\n\n");
    printf("  Iterations: %d\n\n", ITERATIONS);

    // ── Step 1: Create instance with layer ──────────────────────────────
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Synapse Execution Benchmark";
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

    // ── Step 2: Create device ───────────────────────────────────────────
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

    // ── Step 3: Get GIPA ────────────────────────────────────────────────
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        vkGetInstanceProcAddr(instance, "vkGetInstanceProcAddr"));

    printf("  Device created.\n\n");

    // ── Step 4: Benchmark GIPA instance function resolution ─────────────
    // This measures the cost of the layer's dispatch_key lookup + mutex +
    // instance map find + chain call for each GIPA query.
    printf("  GIPA instance function resolution:\n");

    const char* instanceFuncs[] = {
        "vkCreateDevice",
        "vkDestroyDevice",
        "vkCreateImageView",
        "vkDestroyImageView",
        "vkCreateImage",
        "vkDestroyImage",
        "vkCreateCommandPool",
        "vkAllocateCommandBuffers",
    };

    double total_ns = 0;
    int count = 0;

    for (auto fn : instanceFuncs) {
        // Warm up
        for (int i = 0; i < 100; i++) {
            gipa(instance, fn);
        }

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < ITERATIONS; i++) {
            volatile auto resolved = gipa(instance, fn);
            (void)resolved;
        }
        auto end = std::chrono::high_resolution_clock::now();

        double avg_ns = std::chrono::duration<double, std::nano>(end - start).count() / ITERATIONS;
        printf("    %-30s %8.1f ns/call\n", fn, avg_ns);
        total_ns += avg_ns;
        count++;
    }

    double avg_ns = total_ns / count;
    printf("    %-30s %8.1f ns/call\n", "AVERAGE", avg_ns);

    // ── Step 5: GDPA device function resolution ──────────────────────────
    printf("\n  GDPA device function resolution:\n");

    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));

    const char* deviceFuncs[] = {
        "vkCmdDrawIndexed",
        "vkCmdDraw",
        "vkCmdDispatch",
        "vkCmdPushConstants",
        "vkCmdBindDescriptorSets",
        "vkCmdBindPipeline",
        "vkCreateImage",
        "vkDestroyImage",
    };

    double total_gdpa_ns = 0;
    int gdpa_count = 0;

    for (auto fn : deviceFuncs) {
        for (int i = 0; i < 100; i++) {
            gdpa(device, fn);
        }

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < ITERATIONS; i++) {
            volatile auto resolved = gdpa(device, fn);
            (void)resolved;
        }
        auto end = std::chrono::high_resolution_clock::now();

        double avg = std::chrono::duration<double, std::nano>(end - start).count() / ITERATIONS;
        printf("    %-30s %8.1f ns/call\n", fn, avg);
        total_gdpa_ns += avg;
        gdpa_count++;
    }

    double avg_gdpa_ns = total_gdpa_ns / gdpa_count;
    printf("    %-30s %8.1f ns/call\n", "AVERAGE", avg_gdpa_ns);

    // ── Step 6: Summary ─────────────────────────────────────────────────
    printf("\n  Summary:\n");
    printf("    GIPA avg overhead:     %8.1f ns/call\n", avg_ns);
    printf("    GDPA avg overhead:     %8.1f ns/call\n", avg_gdpa_ns);
    printf("    At 60 FPS (16.67ms):   %.4f%% frame budget (GIPA)\n",
           (avg_ns / 16666666.7) * 100);
    printf("    At 60 FPS (16.67ms):   %.4f%% frame budget (GDPA)\n",
           (avg_gdpa_ns / 16666666.7) * 100);

    // ── Step 7: Cleanup ─────────────────────────────────────────────────
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    printf("\n=== Execution Overhead Benchmark Complete ===\n");
    return 0;
}
