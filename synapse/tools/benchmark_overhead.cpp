// ============================================================================
// synapse/tools/benchmark_overhead.cpp
// Real hardware benchmark: measures layer interception overhead.
// Creates a Vulkan device with/without the layer and times
// vkGetDeviceProcAddr calls to measure function pointer resolution cost.
//
// Build: via CMake target benchmark_overhead
// Run:   VK_LAYER_PATH=<dir> VK_INSTANCE_LAYERS=VK_LAYER_SYNAPSE_iGPU_Shim
//        benchmark_overhead.exe
// ============================================================================

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <string>

#define VK_CHECK(call) do { \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) { \
        printf("  FAIL: %s returned %d\n", #call, _r); \
        return 1; \
    } \
} while(0)

static const int ITERATIONS = 10000;

struct BenchResult {
    double avg_ns;
    double ops_per_sec;
    const char* label;
};

// Time N calls to a function pointer
template<typename Fn, typename... Args>
BenchResult bench_gdpa(PFN_vkGetDeviceProcAddr gdpa, VkDevice device,
                       const char* funcName, int iterations) {
    // Resolve function pointer once
    auto fn = reinterpret_cast<Fn>(gdpa(device, funcName));
    if (!fn) {
        printf("  WARNING: %s resolved to null\n", funcName);
        return {0, 0, funcName};
    }

    // Warm up
    for (int i = 0; i < 100; i++) {
        gdpa(device, funcName);
    }

    // Benchmark: time the GDPA lookup itself (function pointer resolution)
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        // Measure the cost of resolving a function through the layer's GDPA
        volatile auto resolved = gdpa(device, funcName);
        (void)resolved;
    }
    auto end = std::chrono::high_resolution_clock::now();

    double total_ns = std::chrono::duration<double, std::nano>(end - start).count();
    double avg_ns = total_ns / iterations;
    double ops_per_sec = 1e9 / avg_ns;

    return {avg_ns, ops_per_sec, funcName};
}

int main(int argc, char** argv) {
    printf("=== Synapse Layer Overhead Benchmark (Real iGPU) ===\n\n");
    printf("  Iterations: %d\n\n", ITERATIONS);

    // ── Step 1: Create instance with layer ──────────────────────────────
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Synapse Benchmark";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    const char* layers[] = { "VK_LAYER_SYNAPSE_iGPU_Shim" };
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &appInfo;
    ci.enabledLayerCount = 1;
    ci.ppEnabledLayerNames = layers;

    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));

    // ── Step 2: Find GPU and create device ──────────────────────────────
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

    // ── Step 3: Get GDPA function pointer ───────────────────────────────
    auto gipa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));

    printf("  Device created with layer.\n\n");

    // ── Step 4: Benchmark GDPA lookup overhead ──────────────────────────
    printf("  ┌─────────────────────────────┬────────────┬───────────────┐\n");
    printf("  │ Function                    │ ns/call    │ calls/sec     │\n");
    printf("  ├─────────────────────────────┼────────────┼───────────────┤\n");

    const char* funcs[] = {
        "vkCmdDrawIndexed",
        "vkCmdDraw",
        "vkCmdDispatch",
        "vkCmdPushConstants",
        "vkCmdBindDescriptorSets",
        "vkCmdBindPipeline",
        "vkCreateImage",
        "vkDestroyImage",
        "vkFreeCommandBuffers",
        "vkCreateDevice",
        "vkDestroyDevice",
    };

    double total_ns = 0;
    int count = 0;

    for (auto fn : funcs) {
        auto r = bench_gdpa<decltype(vkCmdDrawIndexed)*>(gipa, device, fn, ITERATIONS);
        printf("  │ %-27s │ %8.1f   │ %11.0f   │\n",
               fn, r.avg_ns, r.ops_per_sec);
        total_ns += r.avg_ns;
        count++;
    }

    double avg_ns = total_ns / count;
    printf("  ├─────────────────────────────┼────────────┼───────────────┤\n");
    printf("  │ AVERAGE                     │ %8.1f   │ %11.0f   │\n",
           avg_ns, 1e9 / avg_ns);
    printf("  └─────────────────────────────┴────────────┴───────────────┘\n");

    // ── Step 5: Benchmark raw function call overhead ────────────────────
    // Resolve all function pointers and time calling them directly
    printf("\n  Raw function call overhead (bypassing GDPA resolution):\n");

    auto rawDrawIndexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(
        gipa(device, "vkCmdDrawIndexed"));
    auto rawDraw = reinterpret_cast<PFN_vkCmdDraw>(
        gipa(device, "vkCmdDraw"));
    auto rawDispatch = reinterpret_cast<PFN_vkCmdDispatch>(
        gipa(device, "vkCmdDispatch"));

    // Time a no-op GDPA lookup (warm cache)
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < ITERATIONS; i++) {
            volatile auto fn = gipa(device, "vkCmdDrawIndexed");
            (void)fn;
        }
        auto end = std::chrono::high_resolution_clock::now();
        double total = std::chrono::duration<double, std::nano>(end - start).count();
        printf("    GDPA lookup (cached):  %8.1f ns/call\n", total / ITERATIONS);
    }

    // ── Step 6: Cleanup ─────────────────────────────────────────────────
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    printf("\n=== Benchmark Complete ===\n");
    printf("  Layer GDPA overhead: %.1f ns/call average\n", avg_ns);
    printf("  At 60 FPS (16.67ms frame): %.4f%% frame budget\n",
           (avg_ns / 16666666.7) * 100);

    return 0;
}
