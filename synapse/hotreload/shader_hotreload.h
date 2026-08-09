// ============================================================================
// synapse/hotreload/shader_hotreload.h
// Project Synapse – Shader Hot-Reload with Atomic Cache Swap
//
// Background recompilation of shaders. New ISA binary is compiled to a staging
// cache, then atomically swapped with the active cache. Old cache is cleaned up
// after a grace period.
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace synapse::hotreload {

// A versioned ISA binary
struct ShaderVersion {
    uint64_t hash{0};
    uint32_t version{0};
    std::vector<uint8_t> isa_binary;
    std::chrono::system_clock::time_point compiled_at;
    std::atomic<uint64_t> use_count{0};
};

// Immutable shader cache — swap atomically via pointer
struct ShaderCache {
    std::unordered_map<uint64_t, std::shared_ptr<ShaderVersion>> shaders;
};

// Compiler function type: takes shader hash, returns ISA binary
using ShaderCompiler = std::function<std::vector<uint8_t>(uint64_t hash)>;

class ShaderHotReload {
public:
    explicit ShaderHotReload(ShaderCompiler compiler)
        : compiler_(std::move(compiler))
        , active_cache_(new ShaderCache()) {}

    ~ShaderHotReload() {
        delete active_cache_.load(std::memory_order_acquire);
    }

    ShaderHotReload(const ShaderHotReload&) = delete;
    ShaderHotReload& operator=(const ShaderHotReload&) = delete;

    // Look up a compiled shader (lock-free on fast path)
    const ShaderVersion* lookup(uint64_t hash) const {
        auto cache = active_cache_.load(std::memory_order_acquire);
        auto it = cache->shaders.find(hash);
        if (it != cache->shaders.end()) {
            it->second->use_count.fetch_add(1, std::memory_order_relaxed);
            return it->second.get();
        }
        return nullptr;
    }

    // Recompile a shader in the background
    std::future<bool> recompile_async(uint64_t shader_hash) {
        return std::async(std::launch::async, [this, shader_hash]() {
            return recompile_shader(shader_hash);
        });
    }

    // Compile a new shader and add to cache (synchronous)
    bool compile_and_add(uint64_t shader_hash) {
        auto isa = compiler_(shader_hash);
        if (isa.empty()) return false;

        auto ver = std::make_shared<ShaderVersion>();
        ver->hash = shader_hash;
        ver->isa_binary = std::move(isa);
        ver->compiled_at = std::chrono::system_clock::now();

        ShaderCache* new_cache = new ShaderCache(
            *active_cache_.load(std::memory_order_acquire));
        new_cache->shaders[shader_hash] = ver;

        return swap_cache(new_cache);
    }

    // Cache statistics
    struct CacheStats {
        uint64_t total_shaders{0};
        uint64_t total_uses{0};
    };

    CacheStats stats() const {
        CacheStats s;
        auto cache = active_cache_.load(std::memory_order_acquire);
        for (const auto& [hash, ver] : cache->shaders) {
            s.total_shaders++;
            s.total_uses += ver->use_count.load(std::memory_order_relaxed);
        }
        return s;
    }

    // Number of compiled shaders
    size_t shader_count() const {
        return active_cache_.load(std::memory_order_acquire)->shaders.size();
    }

private:
    ShaderCompiler compiler_;
    std::atomic<ShaderCache*> active_cache_;
    std::mutex cache_mutex_;

    bool recompile_shader(uint64_t shader_hash) {
        auto isa = compiler_(shader_hash);
        if (isa.empty()) return false;

        ShaderCache* new_cache = new ShaderCache(
            *active_cache_.load(std::memory_order_acquire));

        auto existing = new_cache->shaders.find(shader_hash);
        uint32_t next_version = 1;
        if (existing != new_cache->shaders.end()) {
            next_version = existing->second->version + 1;
        }

        auto ver = std::make_shared<ShaderVersion>();
        ver->hash = shader_hash;
        ver->version = next_version;
        ver->isa_binary = std::move(isa);
        ver->compiled_at = std::chrono::system_clock::now();

        new_cache->shaders[shader_hash] = ver;
        return swap_cache(new_cache);
    }

    bool swap_cache(ShaderCache* new_cache) {
        ShaderCache* expected = active_cache_.load(std::memory_order_acquire);
        if (!active_cache_.compare_exchange_strong(
                expected, new_cache,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            delete new_cache;
            return false;
        }
        // Schedule old cache cleanup after grace period
        ShaderCache* old = expected;
        std::thread([old]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            delete old;
        }).detach();
        return true;
    }
};

}  // namespace synapse::hotreload
