#pragma once

#include "contextual_bandit.h"
#include "feature_encoder.h"
#include <string>
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>

namespace synapse::ml {

class MLSubAPI {
public:
    using Features = ContextualBandit::Features;

    MLSubAPI()
        : running_(true), trainer_thread_(&MLSubAPI::trainer_loop, this)
    {}

    ~MLSubAPI() {
        running_.store(false);
        if (trainer_thread_.joinable()) trainer_thread_.join();
    }

    void set_learning_rate(float a) { bandit_.set_learning_rate(a); }
    void set_epsilon(float e) { bandit_.set_epsilon(e); }
    void freeze() { bandit_.freeze(); }
    void unfreeze() { bandit_.unfreeze(); }

    // Checkpoint / restore using bandit serialisation helpers
    void checkpoint(const std::string& path) const {
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) return;
        std::array<float,24> w{};
        bandit_.save_weights(w);
        ofs.write(reinterpret_cast<const char*>(w.data()), sizeof(w));
    }

    void restore(const std::string& path) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return;
        std::array<float,24> w{};
        ifs.read(reinterpret_cast<char*>(w.data()), sizeof(w));
        bandit_.load_weights(w);
    }

    // Make a decision from a WorkloadSignature
    ExecutionBackend decide(const WorkloadSignature& sig) {
        return bandit_.choose_action(encoder_.encode(sig));
    }

    // Observe outcome with features provided externally
    void observe(ExecutionBackend a, float reward, const Features& f) {
        bandit_.observe(a, reward, f);
        // Record experience for replay
        record_experience(f, backend_to_index(a), reward);
    }

    // Observe by passing a WorkloadSignature (encodes internally)
    void observe_from_signature(ExecutionBackend a, float reward, const WorkloadSignature& sig) {
        const Features f = encoder_.encode(sig);
        observe(a, reward, f);
    }

    // Explain
    std::array<float,8> explain_action(int action_index) const { return bandit_.get_weights_for(action_index); }

    // Stats
    uint64_t updates() const { return bandit_.updates(); }
    float cumulative_reward() const { return bandit_.cumulative_reward(); }

private:
    // Experience replay ring
    struct Experience { Features f; int action; float reward; };
    static constexpr size_t kReplaySize = 4096;
    std::array<Experience, kReplaySize> replay_;
    std::atomic<uint64_t> replay_write_{0};
    std::atomic<uint64_t> replay_read_{0};

    void record_experience(const Features& f, int action, float reward) {
        const uint64_t idx = replay_write_.fetch_add(1, std::memory_order_relaxed);
        replay_[idx % kReplaySize] = Experience{f, action, reward};
    }

    void trainer_loop() {
        while (running_.load(std::memory_order_acquire)) {
            uint64_t r = replay_read_.load(std::memory_order_relaxed);
            uint64_t w = replay_write_.load(std::memory_order_acquire);
            while (r < w) {
                const auto& e = replay_[r % kReplaySize];
                // Replay update (may be noisy but strengthens learning)
                bandit_.observe(index_to_backend(e.action), e.reward, e.f);
                r = replay_read_.fetch_add(1, std::memory_order_relaxed) + 1;
            }
            // Sleep briefly to yield CPU — trainer is low priority
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    int backend_to_index(ExecutionBackend b) const {
        switch (b) { case ExecutionBackend::JIT: return 0; case ExecutionBackend::HAI: return 1; default: return 2; }
    }

    ExecutionBackend index_to_backend(int i) const {
        switch (i) { case 0: return ExecutionBackend::JIT; case 1: return ExecutionBackend::HAI; default: return ExecutionBackend::Oracle; }
    }

    ContextualBandit bandit_;
    FeatureEncoder encoder_;

    std::atomic<bool> running_{false};
    std::thread trainer_thread_;
};

} // namespace synapse::ml
