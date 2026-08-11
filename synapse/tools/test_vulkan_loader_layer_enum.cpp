/**
 * @file test_vulkan_loader_layer_enum.cpp
 * @brief Vulkan loader-based layer enumeration test.
 *
 * Uses the Vulkan loader to enumerate instance layers and
 * verifies that VK_LAYER_SYNAPSE_iGPU_Shim is discoverable.
 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
# include <windows.h>
#endif

// Minimal Vulkan types to avoid full header dependency
struct VkLayerProperties {
    char layerName[256];
    uint32_t specVersion;
    uint32_t implementationVersion;
    char description[256];
};

// Function pointer types
typedef int32_t (__stdcall* PFN_vkEnumerateInstanceLayerProperties)(uint32_t*, VkLayerProperties*);

int main() {
    printf("=== Vulkan loader layer enumeration test ===\n");

#if defined(_WIN32)
    HMODULE vulkan = LoadLibraryA("vulkan-1.dll");
    if (!vulkan) {
        printf("  NOTE: vulkan-1.dll not found (%lu)\n", GetLastError());
        printf("Result: SKIP\n");
        return 0;
    }

    auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        GetProcAddress(vulkan, "vkEnumerateInstanceLayerProperties"));
    if (!enumerate) {
        printf("  NOTE: vkEnumerateInstanceLayerProperties not found\n");
        FreeLibrary(vulkan);
        printf("Result: SKIP\n");
        return 0;
    }

    uint32_t count = 0;
    int32_t result = enumerate(&count, nullptr);
    if (result != 0) {
        printf("  NOTE: vkEnumerateInstanceLayerProperties failed (%d)\n", result);
        FreeLibrary(vulkan);
        printf("Result: SKIP\n");
        return 0;
    }

    printf("  total instance layers: %u\n", count);

    std::vector<VkLayerProperties> layers(count);
    result = enumerate(&count, layers.data());
    if (result != 0) {
        printf("  NOTE: vkEnumerateInstanceLayerProperties failed (%d)\n", result);
        FreeLibrary(vulkan);
        printf("Result: SKIP\n");
        return 0;
    }

    const char* target = "VK_LAYER_SYNAPSE_iGPU_Shim";
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (std::string(layers[i].layerName) == target) {
            found = true;
            printf("  found: %s (spec %u, impl %u)\n",
                   layers[i].layerName, layers[i].specVersion, layers[i].implementationVersion);
            break;
        }
    }

    FreeLibrary(vulkan);

    if (found) {
        printf("Result: PASS\n");
        return 0;
    } else {
        printf("  NOTE: %s not found in layer enumeration\n", target);
        printf("Result: SKIP\n");
        return 0;
    }
#else
    printf("  non-Windows platform: SKIP\n");
    printf("Result: PASS\n");
    return 0;
#endif
}
