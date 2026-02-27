// synapse/jit_specialization_cache.h
#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <shared_mutex> // only for the fallback, not used in fast path

namespace synapse {

struct SpecializedShader {
    std::vector<uint32_t> isa;
    uint32_t register_count;
    uint32_t occupancy_hint;
    // ... other metadata
};

class JITSpecializationCache {
public:
    // Called by the render thread (fast path)
    std::shared_ptr<SpecializedShader> get(uint64_t context_hash) {
        // acquire load ensures we see the latest store from the Analyzer
        return cache_[context_hash].load(std::memory_order_acquire);
    }

    // Called by the Analyzer thread (background compilation)
    void insert(uint64_t context_hash, std::shared_ptr<SpecializedShader> shader) {
        // release store makes the new shader visible to all readers
        cache_[context_hash].store(std::move(shader), std::memory_order_release);
    }

private:
    // For simplicity, we use a fixed-size array indexed by a portion of the hash,
    // but a concurrent hash map would be more appropriate. For this example we
    // assume a perfect hash or a small number of entries.
    static constexpr size_t CACHE_SIZE = 1024;
    std::array<std::atomic<std::shared_ptr<SpecializedShader>>, CACHE_SIZE> cache_;
};

} // namespace synapse