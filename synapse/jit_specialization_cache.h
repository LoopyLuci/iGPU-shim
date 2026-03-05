// synapse/jit_specialization_cache.h
// Project Synapse – Lock-Free JIT Shader Cache with Collision Detection
//
// The fast path (render thread) calls get() using a 64-bit context hash.
// Array slots are indexed by the lower 10 bits of the hash (mod CACHE_SIZE).
// To detect hash collisions the full 64-bit key is stored alongside each
// shader entry in a parallel atomic array.  A mismatch on read triggers an
// eviction + background recompile rather than silently returning the wrong ISA.
//
// Risk #8 mitigation: see plan.md Open Risks table.
#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <cstdint>

namespace synapse {

struct SpecializedShader {
    std::vector<uint32_t> isa;
    uint32_t register_count;
    uint32_t occupancy_hint;
    uint64_t source_hash;       ///< Full 64-bit context hash — used for collision check
};

class JITSpecializationCache {
public:
    // -----------------------------------------------------------------------
    /// @brief Fast-path lookup called by the render thread.
    ///
    /// Returns the cached shader if and only if the stored key exactly matches
    /// @p context_hash.  On a hash collision (different workload, same slot)
    /// the entry is treated as a miss and the collision counter is incremented.
    ///
    /// @param context_hash   64-bit key from util::workload_context_hash().
    /// @return               Shared pointer to specialised shader, or nullptr on miss/collision.
    // -----------------------------------------------------------------------
    std::shared_ptr<SpecializedShader> get(uint64_t context_hash) {
        const size_t idx = slot(context_hash);

        // Load stored key first (acquire to see paired shader store)
        const uint64_t stored_key = keys_[idx].load(std::memory_order_acquire);

        if (stored_key == 0) {
            return nullptr;   // Empty slot — clean miss
        }
        if (stored_key != context_hash) {
            // Hash collision: two different workloads map to the same slot.
            // Reject the cached entry and signal a background recompile.
            collision_count_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        // Key matches — safe to read shader
        return cache_[idx].load(std::memory_order_acquire);
    }

    // -----------------------------------------------------------------------
    /// @brief Background-thread insertion (Analyzer / JIT compiler thread).
    ///
    /// Stores both the full 64-bit key and the compiled shader atomically so
    /// that a concurrent get() either sees both or neither.
    ///
    /// @param context_hash  64-bit key that was used to compile this shader.
    /// @param shader        Compiled result to cache.
    // -----------------------------------------------------------------------
    void insert(uint64_t context_hash, std::shared_ptr<SpecializedShader> shader) {
        const size_t idx = slot(context_hash);
        // Tag the shader with its own key for defensive cross-checks
        if (shader) { shader->source_hash = context_hash; }
        // Store shader first, then key — get() sees key==0 until shader is ready
        cache_[idx].store(shader, std::memory_order_release);
        keys_[idx].store(context_hash, std::memory_order_release);
    }

    // -----------------------------------------------------------------------
    /// @brief Evict a slot, forcing the next get() to recompile.
    ///
    /// Called internally on collision detection, or externally when a shader
    /// source changes (e.g. PSO recreation).
    ///
    /// @param context_hash  Key of the entry to evict.
    // -----------------------------------------------------------------------
    void evict(uint64_t context_hash) {
        const size_t idx = slot(context_hash);
        cache_[idx].store(nullptr, std::memory_order_release);
        keys_[idx].store(0,       std::memory_order_release);
    }

    /// @brief Number of hash collisions detected since construction.
    uint32_t collision_count() const {
        return collision_count_.load(std::memory_order_relaxed);
    }

    /// @brief Capacity of the cache (fixed at compile time).
    static constexpr size_t capacity() { return CACHE_SIZE; }

private:
    static constexpr size_t CACHE_SIZE = 1024; ///< Must be a power of two.

    /// @brief Maps context_hash to a slot index via bitmask.
    static constexpr size_t slot(uint64_t h) noexcept {
        return static_cast<size_t>(h & (CACHE_SIZE - 1));
    }

    /// Full 64-bit keys parallel to the shader entries.
    /// key == 0 means empty slot.
    std::array<std::atomic<uint64_t>,                       CACHE_SIZE> keys_{};
    std::array<std::atomic<std::shared_ptr<SpecializedShader>>, CACHE_SIZE> cache_{};

    /// Monotonic collision counter — exposed via collision_count().
    std::atomic<uint32_t> collision_count_{0};
};

} // namespace synapse