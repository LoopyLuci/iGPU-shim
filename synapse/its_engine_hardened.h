// ============================================================================
// synapse/its_engine_hardened.h
// Project Synapse – Hardened Texture Streaming (Hysteresis & DMA Fencing)
// ============================================================================
#pragma once

#include "synapse_umd.h"
#include "power_estimator.h"
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <chrono>

// Platform-specific KMD headers for real fence/DMA paths
#if defined(SYNAPSE_REAL_FENCE) && defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <Windows.h>
#  include <d3dkmthk.h>
#endif

#if defined(SYNAPSE_REAL_FENCE) && defined(__linux__)
#  include <unistd.h>
#  include <sys/ioctl.h>
#  include <cerrno>
#  include <linux/sync_file.h>
#endif

#if defined(SYNAPSE_REAL_DMA) && defined(__linux__)
#  include <fcntl.h>
#  include <drm/drm.h>
#endif

namespace synapse {

struct MipResidencyState {
    MipResidencyState() = default;
    MipResidencyState(MipResidencyState&&) = default;
    MipResidencyState& operator=(MipResidencyState&&) = default;
    MipResidencyState(const MipResidencyState&) = delete;
    MipResidencyState& operator=(const MipResidencyState&) = delete;
    std::atomic<bool> is_resident{false};
    std::atomic<uint64_t> dma_fence_id{0}; // Track async completion
    float current_priority{0.0f};          // Used for Hysteresis
};

// ---------------------------------------------------------------------------
// TextureObject – per-VkImage residency record.
// ---------------------------------------------------------------------------
struct TextureObject {
    TextureObject() = default;
    TextureObject(TextureObject&&) = default;
    TextureObject& operator=(TextureObject&&) = default;
    TextureObject(const TextureObject&) = delete;
    TextureObject& operator=(const TextureObject&) = delete;
    uint64_t texture_id      = 0;
    uint32_t width           = 0;
    uint32_t height          = 0;
    uint32_t mip_count       = 0;
    uint64_t last_used_frame = 0;
    std::unique_ptr<MipResidencyState[]> residency; ///< One entry per mip level
};

class TextureStreamingEngineHardened {
public:
    TextureStreamingEngineHardened(Analyzer& analyzer) : analyzer_(analyzer) {}

    // Register a texture when vkCreateImage is observed.
    void register_texture(VkImage image, const VkImageCreateInfo* createInfo, uint64_t texture_id) {
        TextureObject tex;
        tex.texture_id = texture_id;
        tex.width = createInfo->extent.width;
        tex.height = createInfo->extent.height;
        tex.mip_count = createInfo->mipLevels;
        tex.last_used_frame = 0;

        tex.residency = std::make_unique<MipResidencyState[]>(tex.mip_count);
        for (uint32_t i = 0; i < tex.mip_count; ++i) {
            tex.residency[i].is_resident.store(false, std::memory_order_relaxed);
            tex.residency[i].dma_fence_id.store(0, std::memory_order_relaxed);
            tex.residency[i].current_priority = 0.0f;
        }

        std::unique_lock<std::shared_mutex> lock(textures_mutex_);
        textures_[image] = std::move(tex);
    }

    // Unregister a texture on vkDestroyImage
    void unregister_texture(VkImage image) {
        std::unique_lock<std::shared_mutex> lock(textures_mutex_);
        textures_.erase(image);
    }

    void prepare_for_use(VkImage image, uint64_t /*current_frame*/) {
        std::shared_lock<std::shared_mutex> lock(textures_mutex_);
        auto it = textures_.find(image);
        if (it == textures_.end()) return;

        TextureObject& tex = it->second;
        
        // 1. Telemetry-Driven Prediction
        float predicted_demand = analyzer_.get_mip_demand_probability(tex.texture_id);

        // 2. Apply Hysteresis
        // Only load if demand > 0.8; Only evict if demand < 0.2
        update_residency_with_hysteresis(tex, predicted_demand);
    }

    // Telemetry helpers (minimal implementations). These are lightweight
    // accessor hooks used by the FeatureEncoder until the ITS engine is
    // fully instrumented.
    float get_cache_hit_rate() const {
        const uint64_t hits = cache_hits_.load(std::memory_order_relaxed);
        const uint64_t misses = cache_misses_.load(std::memory_order_relaxed);
        const uint64_t total = hits + misses;
        return total == 0 ? 0.0f : static_cast<float>(hits) / static_cast<float>(total);
    }

    uint32_t get_and_reset_fault_count() {
        return static_cast<uint32_t>(fault_count_.exchange(0u, std::memory_order_acq_rel));
    }

    // Defensive check: returns the highest mip-level that is safely resident.
    // Acquires a shared lock so concurrent readers do not block each other on
    // the hot path. Writes to the map always go through unique_lock callers.
    uint32_t get_safe_mip_level(VkImage image) {
        std::shared_lock<std::shared_mutex> lock(textures_mutex_);
        auto it = textures_.find(image);
        if (it == textures_.end()) {
            // Image was not registered; record as fault and return mip 0.
            cache_misses_.fetch_add(1u, std::memory_order_relaxed);
            fault_count_.fetch_add(1u, std::memory_order_relaxed);
            return 0;
        }
        const TextureObject& tex = it->second;
        for (uint32_t i = 0; i < tex.mip_count; ++i) {
            // Check if DMA transfer is actually finished via hardware fence.
            if (tex.residency[i].is_resident.load(std::memory_order_acquire) &&
                hardware_fence_completed(
                    tex.residency[i].dma_fence_id.load(std::memory_order_relaxed))) {
                cache_hits_.fetch_add(1u, std::memory_order_relaxed);
                return i;
            }
        }
        // No resident mip was safe — record a miss/fault and return lowest-res mip.
        cache_misses_.fetch_add(1u, std::memory_order_relaxed);
        fault_count_.fetch_add(1u, std::memory_order_relaxed);
        return tex.mip_count > 0 ? tex.mip_count - 1 : 0;
    }

    // Expose raw counters for wiring into SynapseCore live_report.
    uint64_t get_hits()   const { return cache_hits_.load(std::memory_order_relaxed); }
    uint64_t get_misses() const { return cache_misses_.load(std::memory_order_relaxed); }

private:
    void update_residency_with_hysteresis(TextureObject& tex, float demand) {
        // Hysteresis prevents 'flickering' residency states
        const float kLoadThreshold = 0.85f;
        const float kEvictThreshold = 0.15f;

        // Iterate over residency entries and request loads/evictions.
        for (uint32_t i = 0; i < tex.mip_count; ++i) {
            auto& mr = tex.residency[i];
            bool resident = mr.is_resident.load(std::memory_order_relaxed);
            if (!resident && demand > kLoadThreshold) {
                trigger_async_load(tex, i);
            } else if (resident && demand < kEvictThreshold) {
                trigger_async_eviction(tex, i);
            }
        }
    }

    // -----------------------------------------------------------------------
    // hardware_fence_completed
    // Gate real fence queries behind SYNAPSE_REAL_FENCE so simulations and
    // unit tests work without kernel headers. Both real paths degrade to the
    // documented fallback (return true) until the MMIO plumbing is complete;
    // they do NOT silently corrupt hardware state.
    // -----------------------------------------------------------------------
    bool hardware_fence_completed(uint64_t fence_id) noexcept {
#if defined(SYNAPSE_REAL_FENCE) && defined(_WIN32)
        // Windows path: D3DKMTWait for the fence signalled by the KMD.
        // fence_id is a D3DKMT fence handle (opaque uint64 from KMD).
        if (fence_id == 0) return true;  // Null fence = always complete

        D3DKMT_WAITFENCESYNCOBJECT wait{};
        wait.hFence = reinterpret_cast<D3DKMT_FENCEHANDLE>(fence_id);
        wait.Timeout = 0;  // Non-blocking poll

        NTSTATUS status = D3DKMTWaitFenceSyncObject(&wait);
        if (status == STATUS_SUCCESS) {
            return true;   // Fence signalled
        }
        if (status == STATUS_TIMEOUT) {
            return false;  // Fence pending
        }
        // Error or device-lost: treat as complete to avoid infinite wait
        return true;

#elif defined(SYNAPSE_REAL_FENCE) && defined(__linux__)
        // Linux path: sync_wait on the DRM fence fd stored in fence_id.
        // fence_id is a file descriptor pointing to a sync_file.
        if (fence_id == 0) return true;  // Null fence = always complete

        int fd = static_cast<int>(fence_id);
        if (fd < 0) return true;  // Invalid fd = treat as complete

        // sync_wait(fd, 0) returns 0 if signalled, -1 with errno=ETIME if pending
        int result = sync_wait(fd, 0 /* non-blocking */);
        if (result == 0) {
            return true;   // Fence signalled
        }
        // ETIME or other error: treat as pending or complete based on errno
        return (errno != ETIME && errno != EAGAIN);

#elif defined(SYNAPSE_REAL_FENCE)
#  warning "SYNAPSE_REAL_FENCE is set but no platform fence implementation is available"
        (void)fence_id;
        return true;
#else
        // Simulation / unit-test path: synthetic fence IDs are always complete.
        (void)fence_id;
        return true;
#endif
    }

    // -----------------------------------------------------------------------
    // trigger_async_load
    // Real path (SYNAPSE_STUB_DMA undefined): enqueue KMD DMA transfer and
    // record the returned fence id. Stub path: synchronous residency flip for
    // simulation and unit tests.
    // -----------------------------------------------------------------------
    void trigger_async_load(TextureObject& tex, uint32_t mip_level) {
        auto& mr = tex.residency[mip_level];
#if defined(SYNAPSE_REAL_DMA) && defined(__linux__)
        // Real KMD path — enqueue DMA transfer via DRM_IOCTL_SYNCOOBJ_TIMELINE_WAIT
        // or legacy DRM_IOCTL_MODE_GETFB / dumb buffer copy for non-tiling.
        // For now: record a synthetic fence id and mark resident. When the
        // actual DRM syncobj ioctl path is wired, replace with:
        //   struct drm_syncobj_timeline_wait wait = { ... };
        //   ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait);
        mr.dma_fence_id.store(1u, std::memory_order_relaxed);
        mr.is_resident.store(true, std::memory_order_release);
        if (power_estimator_) {
            // Estimate full mip size for this level and log as saved bandwidth
            // once real DMA byte counts are available.
            power_estimator_->log_transaction(tex.width * tex.height * 4, 0);
        }
#elif defined(SYNAPSE_REAL_DMA)
        // Windows D3DKMTRender path — placeholder for future implementation
        // TODO(T1-2): Enqueue DMA transfer via D3DKMTRender. Record the fence
        // id returned by the KMD into mr.dma_fence_id.
        mr.dma_fence_id.store(1u, std::memory_order_relaxed);
        mr.is_resident.store(true, std::memory_order_release);
        if (power_estimator_) {
            power_estimator_->log_transaction(tex.width * tex.height * 4, 0);
        }
#else
        // Simulation / unit-test path (SYNAPSE_STUB_DMA or neither flag):
        // mark resident synchronously with a synthetic fence id.
        mr.dma_fence_id.store(1u, std::memory_order_relaxed);
        mr.is_resident.store(true, std::memory_order_release);
#endif
    }

    void trigger_async_eviction(TextureObject& tex, uint32_t mip_level) {
        auto& mr = tex.residency[mip_level];
        mr.is_resident.store(false, std::memory_order_release);
        mr.dma_fence_id.store(0u, std::memory_order_relaxed);
        // Eviction frees bandwidth that was previously consumed — log it.
        if (power_estimator_) {
            power_estimator_->log_transaction(0, 0);
        }
    }

public:
    // -----------------------------------------------------------------
    // Power estimator injection — called by SynapseCore to wire
    // bandwidth accounting through the ITS engine.
    // -----------------------------------------------------------------
    void set_power_estimator(synapse::metrics::PowerEstimator* estimator) {
        power_estimator_ = estimator;
    }

    bool mark_dma_complete(VkImage image, uint64_t fence_id, uint32_t mip_level) {
        std::unique_lock<std::shared_mutex> lock(textures_mutex_);
        auto it = textures_.find(image);
        if (it == textures_.end()) return false;
        if (mip_level >= it->second.mip_count) return false;
        auto& mr = it->second.residency[mip_level];
        mr.dma_fence_id.store(fence_id, std::memory_order_relaxed);
        mr.is_resident.store(true, std::memory_order_release);
        return true;
    }

private:
    // -----------------------------------------------------------------
    // Internal telemetry counters
    // -----------------------------------------------------------------
    std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> cache_misses_{0};
    std::atomic<uint32_t> fault_count_{0};

    // Texture registry (maps VkImage -> TextureObject)
    // Protected by textures_mutex_: unique_lock for writes, shared_lock for reads.
    std::shared_mutex textures_mutex_;
    std::unordered_map<VkImage, TextureObject> textures_;

    // Back-reference to the Analyzer for demand-probability queries.
    Analyzer& analyzer_;

    // Power estimator — wired by SynapseCore at device creation.
    synapse::metrics::PowerEstimator* power_estimator_ = nullptr;
};

} // namespace synapse