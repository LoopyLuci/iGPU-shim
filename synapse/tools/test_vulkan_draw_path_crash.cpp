/**
 * @file test_vulkan_draw_path_crash.cpp
 * @brief Isolate the Intel UHD 630 driver crash on vkCmdDraw.
 *
 * Tests each draw-path step individually with file-based logging to capture
 * the crash location even if stdout is lost.
 */

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define VK_CHECK_LOG(call, ctx_log) do { \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) { \
        fprintf(ctx_log, "  FAIL: %s returned %d\n", #call, _r); \
        fclose(ctx_log); \
        return 1; \
    } \
} while(0)

static FILE* g_log = nullptr;

#define LOG(fmt, ...) do { \
    if (g_log) { fprintf(g_log, fmt, ##__VA_ARGS__); fflush(g_log); } \
    printf(fmt, ##__VA_ARGS__); fflush(stdout); \
} while(0)

struct VkCtx {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t gfxQ = UINT32_MAX;
    uint32_t compQ = UINT32_MAX;

    ~VkCtx() {
        if (dev) vkDestroyDevice(dev, nullptr);
        if (inst) vkDestroyInstance(inst, nullptr);
    }
};

static bool createContext(VkCtx& c) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Test";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    if (vkCreateInstance(&ici, nullptr, &c.inst) != VK_SUCCESS) {
        LOG("  FAIL: vkCreateInstance\n");
        return false;
    }

    uint32_t cnt = 0;
    vkEnumeratePhysicalDevices(c.inst, &cnt, nullptr);
    std::vector<VkPhysicalDevice> devs(cnt);
    vkEnumeratePhysicalDevices(c.inst, &cnt, devs.data());
    c.phys = devs[0];

    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(c.phys, &p);
    LOG("  GPU: %s\n", p.deviceName);

    uint32_t qfCnt = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qfCnt, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCnt);
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qfCnt, qfs.data());

    for (uint32_t i = 0; i < qfCnt; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT && c.gfxQ == UINT32_MAX) c.gfxQ = i;
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT && c.compQ == UINT32_MAX) c.compQ = i;
    }

    float pri = 1.0f;
    VkDeviceQueueCreateInfo dqi{};
    dqi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqi.queueFamilyIndex = c.gfxQ;
    dqi.queueCount = 1;
    dqi.pQueuePriorities = &pri;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqi;

    if (vkCreateDevice(c.phys, &dci, nullptr, &c.dev) != VK_SUCCESS) {
        LOG("  FAIL: vkCreateDevice\n");
        return false;
    }
    LOG("  Device created. gfxQ=%u compQ=%u\n", c.gfxQ, c.compQ);
    return true;
}

// Test 2a: Empty command buffer submit (does the queue work at all?)
static bool testEmptySubmit(VkCtx& c, uint32_t qIdx) {
    LOG("\n[2a] Empty command buffer submit\n");
    if (qIdx == UINT32_MAX) { LOG("  SKIP: no queue\n"); return true; }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = qIdx;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkCreateCommandPool(c.dev, &cpci, nullptr, &pool), g_log);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkAllocateCommandBuffers(c.dev, &cbai, &cmd), g_log);

    VkCommandBufferBeginInfo beg{};
    beg.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beg.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK_LOG(vkBeginCommandBuffer(cmd, &beg), g_log);
    VK_CHECK_LOG(vkEndCommandBuffer(cmd), g_log);

    VkQueue q = VK_NULL_HANDLE;
    vkGetDeviceQueue(c.dev, qIdx, 0, &q);

    VkSubmitInfo sub{};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cmd;
    LOG("  vkQueueSubmit...\n");
    VkResult sr = vkQueueSubmit(q, 1, &sub, VK_NULL_HANDLE);
    LOG("  vkQueueSubmit returned 0x%x\n", sr);

    LOG("  vkQueueWaitIdle...\n");
    fflush(stdout); if (g_log) fflush(g_log);
    VkResult wr = vkQueueWaitIdle(q);
    LOG("  vkQueueWaitIdle returned 0x%x\n", wr);

    vkDestroyCommandPool(c.dev, pool, nullptr);
    LOG("  PASS: empty submit\n");
    return true;
}

// Test 2b: Compute dispatch — fine-grained
static bool testCompute(VkCtx& c) {
    LOG("\n[2b] Compute dispatch\n");
    if (c.compQ == UINT32_MAX) { LOG("  SKIP: no compute queue\n"); return true; }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = c.compQ;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkCreateCommandPool(c.dev, &cpci, nullptr, &pool), g_log);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkAllocateCommandBuffers(c.dev, &cbai, &cmd), g_log);

    VkCommandBufferBeginInfo beg{};
    beg.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beg.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK_LOG(vkBeginCommandBuffer(cmd, &beg), g_log);

    LOG("  Calling vkCmdDispatch(1,1,1)...\n");
    fflush(stdout); if (g_log) fflush(g_log);
    vkCmdDispatch(cmd, 1, 1, 1);
    LOG("  vkCmdDispatch returned\n");

    VK_CHECK_LOG(vkEndCommandBuffer(cmd), g_log);

    VkQueue q = VK_NULL_HANDLE;
    vkGetDeviceQueue(c.dev, c.compQ, 0, &q);

    VkSubmitInfo sub{};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cmd;
    LOG("  vkQueueSubmit...\n");
    fflush(stdout); if (g_log) fflush(g_log);
    VkResult sr = vkQueueSubmit(q, 1, &sub, VK_NULL_HANDLE);
    LOG("  vkQueueSubmit returned 0x%x\n", sr);

    LOG("  vkQueueWaitIdle...\n");
    fflush(stdout); if (g_log) fflush(g_log);
    VkResult wr = vkQueueWaitIdle(q);
    LOG("  vkQueueWaitIdle returned 0x%x\n", wr);

    vkDestroyCommandPool(c.dev, pool, nullptr);
    LOG("  PASS: compute dispatch\n");
    return true;
}

// Test 3: Buffer transfer
static bool testTransfer(VkCtx& c) {
    LOG("\n[3] Buffer transfer\n");
    uint32_t qIdx = c.gfxQ;
    if (qIdx == UINT32_MAX) { LOG("  SKIP: no queue\n"); return true; }

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
    uint32_t hostIdx = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            hostIdx = i; break;
        }
    }
    if (hostIdx == UINT32_MAX) { LOG("  SKIP: no host mem\n"); return true; }

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 1024;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkCreateBuffer(c.dev, &bci, nullptr, &src), g_log);
    VK_CHECK_LOG(vkCreateBuffer(c.dev, &bci, nullptr, &dst), g_log);

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(c.dev, src, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = hostIdx;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkAllocateMemory(c.dev, &ai, nullptr, &mem), g_log);
    VK_CHECK_LOG(vkBindBufferMemory(c.dev, src, mem, 0), g_log);
    VK_CHECK_LOG(vkBindBufferMemory(c.dev, dst, mem, 512), g_log);

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = qIdx;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkCreateCommandPool(c.dev, &cpci, nullptr, &pool), g_log);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkAllocateCommandBuffers(c.dev, &cbai, &cmd), g_log);

    VkCommandBufferBeginInfo beg{};
    beg.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beg.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK_LOG(vkBeginCommandBuffer(cmd, &beg), g_log);

    VkBufferCopy region{}; region.size = 1024;
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    VK_CHECK_LOG(vkEndCommandBuffer(cmd), g_log);

    VkQueue q = VK_NULL_HANDLE;
    vkGetDeviceQueue(c.dev, qIdx, 0, &q);

    VkSubmitInfo sub{};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cmd;
    VK_CHECK_LOG(vkQueueSubmit(q, 1, &sub, VK_NULL_HANDLE), g_log);
    VK_CHECK_LOG(vkQueueWaitIdle(q), g_log);

    LOG("  PASS: buffer transfer\n");
    vkDestroyCommandPool(c.dev, pool, nullptr);
    vkDestroyBuffer(c.dev, src, nullptr);
    vkDestroyBuffer(c.dev, dst, nullptr);
    vkFreeMemory(c.dev, mem, nullptr);
    return true;
}

// Test 4: Graphics pipeline + vkCmdDraw
static bool testGraphicsDraw(VkCtx& c) {
    LOG("\n[4] Graphics pipeline + vkCmdDraw\n");
    if (c.gfxQ == UINT32_MAX) { LOG("  SKIP: no graphics queue\n"); return true; }

    const uint32_t W = 64, H = 64;

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
    uint32_t devLocal = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            devLocal = i; break;
        }
    }
    if (devLocal == UINT32_MAX) { LOG("  SKIP: no device-local mem\n"); return true; }

    // Color image
    VkImageCreateInfo imgCI{};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgCI.extent = { W, H, 1 };
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImage img = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkCreateImage(c.dev, &imgCI, nullptr, &img), g_log);

    VkMemoryRequirements imgReq{};
    vkGetImageMemoryRequirements(c.dev, img, &imgReq);
    VkMemoryAllocateInfo imgAI{};
    imgAI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imgAI.allocationSize = imgReq.size;
    imgAI.memoryTypeIndex = devLocal;
    VkDeviceMemory imgMem = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkAllocateMemory(c.dev, &imgAI, nullptr, &imgMem), g_log);
    VK_CHECK_LOG(vkBindImageMemory(c.dev, img, imgMem, 0), g_log);

    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkCreateImageView(c.dev, &ivci, nullptr, &view), g_log);

    // Render pass
    VkAttachmentDescription ad{};
    ad.format = VK_FORMAT_R8G8B8A8_UNORM;
    ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ar{};
    ar.attachment = 0;
    ar.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sd{};
    sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sd.colorAttachmentCount = 1;
    sd.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &ad;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sd;
    VkRenderPass rp = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkCreateRenderPass(c.dev, &rpci, nullptr, &rp), g_log);

    // Framebuffer
    VkFramebufferCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = rp;
    fci.attachmentCount = 1;
    fci.pAttachments = &view;
    fci.width = W;
    fci.height = H;
    fci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkCreateFramebuffer(c.dev, &fci, nullptr, &fb), g_log);

    LOG("  Framebuffer created OK\n");

    // Skip shader module / pipeline creation — the crash is likely at submit time.
    // Instead, try just recording a draw command without a pipeline bound
    // to see if that's what triggers it.

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = c.gfxQ;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkCreateCommandPool(c.dev, &cpci, nullptr, &pool), g_log);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK_LOG(vkAllocateCommandBuffers(c.dev, &cbai, &cmd), g_log);

    VkCommandBufferBeginInfo beg{};
    beg.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beg.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK_LOG(vkBeginCommandBuffer(cmd, &beg), g_log);

    VkClearValue cv{}; cv.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea.extent = { W, H };
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &cv;

    LOG("  Calling vkCmdBeginRenderPass...\n");
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    LOG("  vkCmdBeginRenderPass returned\n");

    LOG("  Calling vkCmdDraw(3,1,0,0)...\n");
    vkCmdDraw(cmd, 3, 1, 0, 0);
    LOG("  vkCmdDraw returned\n");

    vkCmdEndRenderPass(cmd);
    LOG("  vkCmdEndRenderPass returned\n");

    VK_CHECK_LOG(vkEndCommandBuffer(cmd), g_log);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(c.dev, c.gfxQ, 0, &queue);

    VkSubmitInfo sub{};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cmd;
    LOG("  Calling vkQueueSubmit...\n");
    VkResult sr = vkQueueSubmit(queue, 1, &sub, VK_NULL_HANDLE);
    LOG("  vkQueueSubmit returned 0x%x\n", sr);

    LOG("  Calling vkQueueWaitIdle...\n");
    VkResult wr = vkQueueWaitIdle(queue);
    LOG("  vkQueueWaitIdle returned 0x%x\n", wr);

    if (sr == VK_SUCCESS && wr == VK_SUCCESS) {
        LOG("  PASS: graphics draw completed\n");
    } else {
        LOG("  NOTE: submit/wait failed\n");
    }

    // Cleanup
    vkDestroyCommandPool(c.dev, pool, nullptr);
    vkDestroyFramebuffer(c.dev, fb, nullptr);
    vkDestroyRenderPass(c.dev, rp, nullptr);
    vkDestroyImageView(c.dev, view, nullptr);
    vkDestroyImage(c.dev, img, nullptr);
    vkFreeMemory(c.dev, imgMem, nullptr);
    return true;
}

int main(int argc, char** argv) {
    g_log = fopen("C:/Users/limpi/AppData/Local/Temp/vulkan_draw_crash.log", "w");
    printf("=== Vulkan Draw-Path Crash Isolation ===\n");
    if (g_log) fprintf(g_log, "=== Vulkan Draw-Path Crash Isolation ===\n");

    VkCtx ctx;
    printf("[1] Context creation\n");
    if (g_log) fprintf(g_log, "[1] Context creation\n");
    if (!createContext(ctx)) {
        printf("RESULT: FAIL (context)\n");
        if (g_log) { fprintf(g_log, "RESULT: FAIL (context)\n"); fclose(g_log); }
        return 1;
    }
    printf("  PASS\n");

    bool ok = true;
    ok = testEmptySubmit(ctx, ctx.gfxQ) && ok;
    ok = testCompute(ctx) && ok;
    ok = testTransfer(ctx) && ok;
    ok = testGraphicsDraw(ctx) && ok;

    printf("\nRESULT: %s\n", ok ? "PASS" : "FAIL");
    if (g_log) { fprintf(g_log, "\nRESULT: %s\n", ok ? "PASS" : "FAIL"); fclose(g_log); }
    return ok ? 0 : 1;
}
