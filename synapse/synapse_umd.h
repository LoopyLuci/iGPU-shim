// ============================================================================
// synapse/synapse_umd.h
// Project Synapse – UMD Integration Layer (Production Hardened)
// ============================================================================
#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <vector>
#include <new>          // For std::hardware_destructive_interference_size
#include <vulkan/vulkan.h> 

namespace synapse {

// ----------------------------------------------------------------------------
// Telemetry Ring Buffer – Lock‑free, single‑producer single‑consumer.
// ----------------------------------------------------------------------------
struct WorkloadSignature {
    uint32_t draw_call_count;            
    uint32_t pipeline_state_changes;     
    uint32_t shader_instruction_estimate;
    uint32_t vertex_count;               
    uint32_t texture_bindings;            
    bool     is_compute_dispatch;        
    uint64_t shader_hash;           ///< FNV64 of bound pipeline shader paths; 0 = unknown
};

// Use standard cache line size to prevent false sharing between threads
constexpr size_t kCacheLineSize = 
#ifdef __cpp_lib_hardware_interference_size
    std::hardware_destructive_interference_size;
#else
    64;
#endif

class TelemetryRingBuffer {
public:
    static constexpr size_t kBufferSize = 1024;   // Must be power of two

    TelemetryRingBuffer() : head_(0), tail_(0) {}

    bool push(const WorkloadSignature& sample) noexcept {
        const auto current_head = head_.load(std::memory_order_relaxed);
        const auto next_head = (current_head + 1) & (kBufferSize - 1);
        
        // Acquire barrier ensures we see the latest tail_ from the consumer
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;   
        }
        buffer_[current_head] = sample;
        
        // Release barrier ensures the payload is visible before the head update
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    std::optional<WorkloadSignature> pop() noexcept {
        const auto current_tail = tail_.load(std::memory_order_relaxed);
        
        // Acquire barrier ensures we see the latest head_ from the producer
        if (current_tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;    
        }
        WorkloadSignature sample = buffer_[current_tail];
        
        // Release barrier ensures data read is complete before tail update
        tail_.store((current_tail + 1) & (kBufferSize - 1), std::memory_order_release);
        return sample;
    }

private:
    std::array<WorkloadSignature, kBufferSize> buffer_;
    
    // Align to cache boundaries to prevent false sharing
    alignas(kCacheLineSize) std::atomic<size_t> head_;      // Producer index
    alignas(kCacheLineSize) std::atomic<size_t> tail_;      // Consumer index
};

// ----------------------------------------------------------------------------
// Execution Backend Types
// ----------------------------------------------------------------------------
enum class ExecutionBackend {
    JIT,        
    HAI,        
    Oracle      
};

// ----------------------------------------------------------------------------
// Analyzer – Consumes telemetry and infers optimal backend.
// ----------------------------------------------------------------------------
class Analyzer {
public:
    Analyzer(TelemetryRingBuffer& telemetry) : telemetry_(telemetry) {}

    void process_telemetry_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            if (auto sample = telemetry_.pop()) {
                update_model(*sample);
            } else {
                std::this_thread::yield();
            }
        }
    }

    ExecutionBackend current_recommendation() const noexcept {
        return inferred_backend_.load(std::memory_order_relaxed);
    }

    void shutdown() { running_.store(false, std::memory_order_relaxed); }

    /// @brief Called by SynapseCore each frame with fresh ITS counters so that
    ///        get_mip_demand_probability() returns a data-driven estimate.
    void update_its_stats(uint64_t hits, uint64_t misses) noexcept {
        its_hits_.store(hits,   std::memory_order_relaxed);
        its_misses_.store(misses, std::memory_order_relaxed);
    }

    /// @brief Laplace-smoothed ITS hit rate as mip-demand probability.
    /// Formula: (H+1)/(H+M+2) — equals 0.5 on cold start, converges to the
    /// observed cache-hit rate as the session accumulates evidence.
    float get_mip_demand_probability(uint64_t /*texture_id*/) const noexcept {
        const uint64_t h = its_hits_.load(std::memory_order_relaxed);
        const uint64_t m = its_misses_.load(std::memory_order_relaxed);
        return static_cast<float>(h + 1u) / static_cast<float>(h + m + 2u);
    }

    WorkloadSignature get_last_known_signature() const noexcept {
        return last_signature_;
    }

private:
    void update_model(const WorkloadSignature& sample) {
        ExecutionBackend recommendation = ExecutionBackend::JIT;

        if (sample.shader_instruction_estimate > 1000 || sample.is_compute_dispatch) {
            recommendation = ExecutionBackend::JIT;
        }
        else if (sample.draw_call_count > 100 && sample.shader_instruction_estimate < 200) {
            recommendation = ExecutionBackend::HAI;
        }
        else {
            recommendation = ExecutionBackend::Oracle;
        }

        inferred_backend_.store(recommendation, std::memory_order_relaxed);
        last_signature_ = sample;
    }

private:
    TelemetryRingBuffer& telemetry_;
    std::atomic<ExecutionBackend> inferred_backend_{ExecutionBackend::Oracle};
    std::atomic<bool> running_{true};
    WorkloadSignature last_signature_{};
    // ITS hit/miss counters fed by SynapseCore::handle_draw_indexed each frame.
    std::atomic<uint64_t> its_hits_{0};
    std::atomic<uint64_t> its_misses_{0};
};

// ----------------------------------------------------------------------------
// Scheduler – Decision Engine
// ----------------------------------------------------------------------------
class Scheduler {
public:
    Scheduler(Analyzer& analyzer) : analyzer_(analyzer) {}

    ExecutionBackend decide_backend(const WorkloadSignature& current_workload) {
        ExecutionBackend recommendation = analyzer_.current_recommendation();

        if (oracle_mode_active_) {
            return ExecutionBackend::Oracle;
        }

        // Failsafe validation
        if (recommendation == ExecutionBackend::JIT &&
            current_workload.shader_instruction_estimate < 50 &&
            current_workload.draw_call_count > 500) {
            recommendation = ExecutionBackend::HAI;
        }

        if (detect_performance_regression()) {
            oracle_mode_active_ = true;   
        }

        return recommendation;
    }

    void report_execution_stats(ExecutionBackend used_backend, uint64_t cycles_spent) {
        // Implementation for rolling average telemetry
    }

private:
    bool detect_performance_regression() const {
        return false;
    }

    Analyzer& analyzer_;
    bool oracle_mode_active_{false};   
};

// ----------------------------------------------------------------------------
// UMD Hook – Vulkan Interception & Batching
// ----------------------------------------------------------------------------
struct BatchContext {
    WorkloadSignature accumulated_signature{};
    std::vector<VkCommandBuffer> pending_buffers;
    
    void reset() {
        accumulated_signature = {};
        pending_buffers.clear();
    }
};

class SynapseUMDHook {
public:
    SynapseUMDHook(PFN_vkCmdDrawIndexed original_draw_indexed,
                   PFN_vkCmdDispatch original_dispatch)
        : orig_draw_indexed_(original_draw_indexed)
        , orig_dispatch_(original_dispatch)
        , analyzer_(telemetry_)
        , scheduler_(analyzer_)
    {
        analyzer_thread_ = std::thread(&Analyzer::process_telemetry_loop, &analyzer_);
    }

    ~SynapseUMDHook() {
        analyzer_.shutdown();
        if (analyzer_thread_.joinable())
            analyzer_thread_.join();
    }

    void vkCmdDrawIndexed(
        VkCommandBuffer commandBuffer,
        uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndex,
        int32_t  vertexOffset,
        uint32_t firstInstance)
    {
        // 1. Accumulate state into current batch context instead of immediate submission
        current_batch_.accumulated_signature.draw_call_count++;
        current_batch_.accumulated_signature.vertex_count += indexCount;
        current_batch_.accumulated_signature.shader_instruction_estimate += estimate_shader_complexity(commandBuffer);
        
        // Note: In a full implementation, we flush/submit the batch based on state changes 
        // (e.g., vkCmdBindPipeline) or render pass boundaries, not per-draw call.
        
        // For demonstration, we simulate executing the batch here if a threshold is hit.
        if (current_batch_.accumulated_signature.draw_call_count > 100 /* threshold */) {
            flush_batch();
        }
        
        // Execute original logic for state tracking only (Oracle bypasses this)
        orig_draw_indexed_(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void vkCmdDispatch(
        VkCommandBuffer commandBuffer,
        uint32_t groupCountX,
        uint32_t groupCountY,
        uint32_t groupCountZ)
    {
        // Compute dispatches usually warrant an immediate flush of pending graphics batches
        flush_batch();
        
        WorkloadSignature sample;
        sample.is_compute_dispatch = true;
        sample.draw_call_count = 1;
        sample.shader_instruction_estimate = estimate_compute_shader_complexity(commandBuffer);
        
        telemetry_.push(sample);

        ExecutionBackend backend = scheduler_.decide_backend(sample);
        if (backend == ExecutionBackend::Oracle) {
            orig_dispatch_(commandBuffer, groupCountX, groupCountY, groupCountZ);
        } else {
            submit_to_synaptic_engine(commandBuffer, backend, sample);
        }
    }

private:
    void flush_batch() {
        if (current_batch_.accumulated_signature.draw_call_count == 0) return;
        
        telemetry_.push(current_batch_.accumulated_signature);
        ExecutionBackend backend = scheduler_.decide_backend(current_batch_.accumulated_signature);
        
        // Submit logic...
        
        current_batch_.reset();
    }

    uint32_t estimate_shader_complexity(VkCommandBuffer) { return 100; }
    uint32_t estimate_compute_shader_complexity(VkCommandBuffer) { return 500; }
    uint32_t count_bound_textures(VkCommandBuffer) { return 2; }

    void submit_to_synaptic_engine(VkCommandBuffer, ExecutionBackend, const WorkloadSignature&) {
        // Submits to JIT or HAI
    }

    PFN_vkCmdDrawIndexed orig_draw_indexed_;
    PFN_vkCmdDispatch    orig_dispatch_;

    TelemetryRingBuffer telemetry_;
    Analyzer            analyzer_;
    Scheduler           scheduler_;
    std::thread         analyzer_thread_;
    BatchContext        current_batch_;
};

} // namespace synapse