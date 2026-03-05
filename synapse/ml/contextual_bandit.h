#pragma once

#include <array>
#include <random>
#include <atomic>
#include <mutex>
#include "synapse_umd.h"

namespace synapse::ml {

class ContextualBandit {
public:
    using Features = std::array<float,8>;

    ContextualBandit()
        : epsilon_(0.05f), alpha_(0.01f), rng_(std::random_device{}())
    {
        // Initialize weights to small random values
        std::uniform_real_distribution<float> d(-0.01f, 0.01f);
        for (int a = 0; a < 3; ++a) for (int j = 0; j < 8; ++j) weights_[a][j] = d(rng_);
    }

    // Hot-path inference: no heap, simple dot-products.
    ExecutionBackend choose_action(const Features& f) {
        // Epsilon-greedy
        if (random_float() < epsilon_) {
            return random_action();
        }
        float scores[3]{};
        for (int a = 0; a < 3; ++a) {
            float s = 0.0f;
            for (int j = 0; j < 8; ++j) s += weights_[a][j] * f[j];
            scores[a] = s;
        }
        int best = 0;
        if (scores[1] > scores[best]) best = 1;
        if (scores[2] > scores[best]) best = 2;
        return index_to_backend(best);
    }

    // Observe outcome and perform one-step gradient update (online).
    void observe(ExecutionBackend action, float reward, const Features& f) {
        const int a = backend_to_index(action);
        std::lock_guard<std::mutex> lg(mu_);
        // Predict current value
        float pred = 0.0f;
        for (int j = 0; j < 8; ++j) pred += weights_[a][j] * f[j];
        const float err = reward - pred;
        for (int j = 0; j < 8; ++j) weights_[a][j] += alpha_ * err * f[j];
        updates_++;
        cumulative_reward_ += reward;
        selection_counts_[a]++;
    }

    // Control surface
    void set_learning_rate(float a) { alpha_ = a; }
    void set_epsilon(float e) { epsilon_ = e; }
    void freeze() { frozen_.store(true); }
    void unfreeze() { frozen_.store(false); }

    // Explainability: return current weight vector for an action index
    std::array<float,8> get_weights_for(int action_index) const {
        std::array<float,8> out{};
        for (int j = 0; j < 8; ++j) out[j] = weights_[action_index][j];
        return out;
    }

    uint64_t updates() const { return updates_; }
    float cumulative_reward() const { return cumulative_reward_; }
    std::array<uint64_t,3> selection_counts() const { return selection_counts_; }

    // -----------------------------------------------------------------------
    // Weight serialisation helpers
    // -----------------------------------------------------------------------
    void save_weights(std::array<float, 24>& out) const {
        std::lock_guard<std::mutex> lg(mu_);
        int idx = 0;
        for (int a = 0; a < 3; ++a) for (int j = 0; j < 8; ++j) out[idx++] = weights_[a][j];
    }

    void load_weights(const std::array<float,24>& in) {
        std::lock_guard<std::mutex> lg(mu_);
        int idx = 0;
        for (int a = 0; a < 3; ++a) for (int j = 0; j < 8; ++j) weights_[a][j] = in[idx++];
    }

private:
    float random_float() { return std::uniform_real_distribution<float>(0.0f,1.0f)(rng_); }
    ExecutionBackend random_action() { return index_to_backend(static_cast<int>(rng_()%3)); }
    ExecutionBackend index_to_backend(int i) const {
        switch (i) { case 0: return ExecutionBackend::JIT; case 1: return ExecutionBackend::HAI; default: return ExecutionBackend::Oracle; }
    }
    int backend_to_index(ExecutionBackend b) const {
        switch (b) { case ExecutionBackend::JIT: return 0; case ExecutionBackend::HAI: return 1; default: return 2; }
    }

    float weights_[3][8];
    float epsilon_;
    float alpha_;
    std::mt19937 rng_;
    std::mutex mu_;
    std::atomic<bool> frozen_{false};
    std::atomic<uint64_t> updates_{0};
    std::atomic<float> cumulative_reward_{0.0f};
    std::array<uint64_t,3> selection_counts_{{0,0,0}};
};

} // namespace synapse::ml
