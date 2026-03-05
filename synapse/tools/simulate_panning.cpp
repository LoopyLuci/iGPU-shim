// Simple simulation that registers textures and exercises the ITS engine
#include "../its_engine_hardened.h"
#include "../synapse_umd.h"
#include <vector>
#include <iostream>

int main(int argc, char** argv) {
    using namespace synapse;

    const int atlas_dim = 16;
    const int view_dim = 4;
    const int frames = 200;

    TelemetryRingBuffer telemetry;
    Analyzer analyzer(telemetry);
    TextureStreamingEngineHardened its(analyzer);

    // Register textures
    std::vector<VkImage> images;
    images.reserve(atlas_dim * atlas_dim + 1);
    for (int i = 0; i < atlas_dim * atlas_dim; ++i) {
        VkImage img = reinterpret_cast<VkImage>(static_cast<uintptr_t>(i + 1));
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.extent.width = 256;
        info.extent.height = 256;
        info.mipLevels = 8;
        its.register_texture(img, &info, static_cast<uint64_t>(i));
        images.push_back(img);
    }
    // Skybox
    VkImage sky = reinterpret_cast<VkImage>(static_cast<uintptr_t>(999 + 1));
    VkImageCreateInfo skyinfo{};
    skyinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    skyinfo.extent.width = 4096;
    skyinfo.extent.height = 4096;
    skyinfo.mipLevels = 1;
    its.register_texture(sky, &skyinfo, 999);
    images.push_back(sky);

    int x = 0, y = 0;
    for (int f = 0; f < frames; ++f) {
        if (f > 0 && f % 10 == 0) {
            if (x + view_dim < atlas_dim) x += 1;
            else if (y + view_dim < atlas_dim) { x = 0; y += 1; }
        }

        std::vector<VkImage> active;
        for (int dy = 0; dy < view_dim; ++dy) {
            for (int dx = 0; dx < view_dim; ++dx) {
                int idx = (y + dy) * atlas_dim + (x + dx);
                active.push_back(images[idx]);
            }
        }
        active.push_back(sky);

        // Simulate preparing textures and then sampling safe mip level
        for (auto img : active) {
            its.prepare_for_use(img, static_cast<uint64_t>(f));
            uint32_t mip = its.get_safe_mip_level(img);
            (void)mip;
        }
    }

    float hit_rate = its.get_cache_hit_rate();
    uint32_t faults = its.get_and_reset_fault_count();

    std::cout << "Simulation complete. ITS cache hit rate=" << hit_rate
              << " faults=" << faults << "\n";

    return 0;
}
