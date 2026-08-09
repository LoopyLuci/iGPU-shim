// ============================================================================
// synapse/hotreload/ml_hotreload.h
// Project Synapse – ML Model Hot-Reload with Atomic Swap
//
// Double-buffered model storage: load new model to staging area,
// atomically swap with active model. Old model kept for grace period.
// Supports A/B testing by routing percentage of traffic to previous model.
// ============================================================================
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace synapse::hotreload {

// A generic model interface for hot-reload
// Implementations must provide: load(), decide(), stats()
template<typename ModelType>
class AtomicModelSwap {
public:
    struct ModelVersion {
        uint64_t version{0};
        std::shared_ptr<ModelType> model;
        std::chrono::system_clock::time_point loaded_at;
        std::atomic<uint64_t> inference_count{0};
        std::atomic<double> total_reward{0.0};
    };

    static constexpr size_t kMaxVersions = 4;  // Keep last N versions

    AtomicModelSwap() {
        // Initialize with empty model slots
        for (size_t i = 0; i < kMaxVersions; ++i) {
            slots_[i] = std::make_shared<ModelVersion>();
        }
    }

    // Load new model in background thread
    std::future<bool> schedule_reload(
            std::function<std::shared_ptr<ModelType>()> loader) {
        return std::async(std::launch::async, [this, loader = std::move(loader)]() {
            return load_model_async(loader);
        });
    }

    // Get current model for inference (lock-free read)
    std::shared_ptr<ModelType> current() const {
        auto ver = current_version_.load(std::memory_order_acquire);
        return slots_[ver % kMaxVersions]->model;
    }

    // A/B testing: route a percentage of traffic to previous model
    std::shared_ptr<ModelType> decide_with_ab(double test_pct = 0.1) {
        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<double> dist(0.0, 1.0);

        if (dist(rng) < test_pct) {
            auto prev = previous_version_.load(std::memory_order_acquire);
            if (prev != current_version_.load(std::memory_order_acquire)) {
                return slots_[prev % kMaxVersions]->model;
            }
        }
        return current();
    }

    // Record inference result for the current model
    void record_inference(double reward) {
        auto ver = current_version_.load(std::memory_order_relaxed);
        auto& slot = slots_[ver % kMaxVersions];
        slot->inference_count.fetch_add(1, std::memory_order_relaxed);
        slot->total_reward.fetch_add(reward, std::memory_order_relaxed);
    }

    // Statistics
    struct ModelStats {
        uint64_t version;
        uint64_t inference_count;
        double avg_reward;
        bool loaded;
    };

    ModelStats stats() const {
        auto ver = current_version_.load(std::memory_order_acquire);
        auto& slot = slots_[ver % kMaxVersions];
        ModelStats s{};
        s.version = slot->version;
        s.inference_count = slot->inference_count.load(std::memory_order_relaxed);
        auto total_r = slot->total_reward.load(std::memory_order_relaxed);
        s.avg_reward = s.inference_count > 0 ? total_r / static_cast<double>(s.inference_count) : 0.0;
        s.loaded = slot->model != nullptr;
        return s;
    }

    // Current model version
    uint64_t version() const { return current_version_.load(std::memory_order_acquire); }

private:
    std::array<std::shared_ptr<ModelVersion>, kMaxVersions> slots_;
    std::atomic<uint64_t> current_version_{0};
    std::atomic<uint64_t> previous_version_{0};
    std::mutex load_mutex_;

    bool load_model_async(std::function<std::shared_ptr<ModelType>()> loader) {
        // Load model (may be slow — runs in background thread)
        auto model = loader();
        if (!model) return false;

        // Validate
        if (!validate_model(model)) return false;

        // Create new version entry
        auto new_ver = std::make_shared<ModelVersion>();
        new_ver->version = current_version_.load(std::memory_order_relaxed) + 1;
        new_ver->model = model;
        new_ver->loaded_at = std::chrono::system_clock::now();

        // Atomic swap
        {
            std::lock_guard lock(load_mutex_);
            auto old_ver = current_version_.load(std::memory_order_relaxed);
            previous_version_.store(old_ver, std::memory_order_relaxed);

            auto slot = new_ver->version % kMaxVersions;
            slots_[slot] = new_ver;

            current_version_.store(new_ver->version, std::memory_order_release);
        }

        return true;
    }

    bool validate_model(const std::shared_ptr<ModelType>& model) {
        // Basic sanity: model must be non-null
        // In production: run test inferences, check weight ranges, etc.
        return model != nullptr;
    }
};

}  // namespace synapse::hotreload
