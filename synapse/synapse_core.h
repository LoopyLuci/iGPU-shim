// ============================================================================
// synapse/synapse_core.h
// Project Synapse – Unified Driver Integration Hub
// ============================================================================
#pragma once

#include "synapse_umd.h"
#include "synapse_jit_backend.h"
#include "synapse_hai_builder.h"
#include "its_engine_hardened.h"
#include "hash_utils.h"     // canonical workload_context_hash — DRY fix
#include "ml/ml_sub_api.h"
#include "ml/reward_calculator.h"
#include "power_estimator.h"
#include <chrono>
#include <algorithm>

namespace synapse {

/**
 * @class SynapseCore
 * @brief The central coordinator for Project Synapse. 
 * * This class lives inside the UMD and intercepts Vulkan/D3D12 calls. 
 * It manages the lifecycle of the Analyzer thread and routes work between 
 * the Oracle (legacy), JIT (complex), and HAI (breadth) paths.
 */
class SynapseCore {
public:
    SynapseCore(PFN_vkCmdDrawIndexed orig_draw)
        : orig_draw_indexed_(orig_draw),
          analyzer_(telemetry_),
          scheduler_(analyzer_),
          jit_pipeline_(analyzer_),
          its_engine_(analyzer_)
    {
        // Start background worker
        analyzer_thread_ = std::thread(&Analyzer::process_telemetry_loop, &analyzer_);
    }

    ~SynapseCore() {
        analyzer_.shutdown();
        if (analyzer_thread_.joinable()) analyzer_thread_.join();
    }

    /// @brief Returns JIT cold-cache stutter telemetry for reporting.
    const JITStutterStats& jit_stutter_stats() const { return jit_stats_; }

    // ------------------------------------------------------------------------
    /// @brief Main interception entry point — called instead of vkCmdDrawIndexed.
    ///
    /// Executes the full Synapse critical path:
    ///   1. Captures @p WorkloadSignature and pushes it to @p TelemetryRingBuffer (lock-free).
    ///   2. Calls ITS to ensure required mip levels are resident or queued.
    ///   3. Invokes @p Scheduler to decide JIT / HAI / Oracle backend.
    ///   4. Dispatches to the selected backend.
    ///
    /// @param cmd           Vulkan command buffer the draw is recorded into.
    /// @param indexCount    Index count from the application draw call.
    /// @param instanceCount Instance count from the application draw call.
    /// @param firstIndex    First index offset from the application draw call.
    ///
    /// @note Target latency: p99 ≤ 1 µs end-to-end (see bench_critical_path.cpp).
    // ------------------------------------------------------------------------
    void handle_draw_indexed(
        VkCommandBuffer cmd,
        uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndex)
    {
        // 1. Telemetry Aggregation (Lock-Free)
        WorkloadSignature sig = capture_current_signature(cmd, indexCount);
        telemetry_.push(sig);

        // 2. Memory & Bandwidth Optimization (ITS)
        // Ensure required mips are resident or queued for DMA
        its_engine_.prepare_for_use(get_bound_image(cmd), current_frame_);

        // 3. Backend Decision (ML contextual bandit)
        const auto t0 = std::chrono::high_resolution_clock::now();
        ExecutionBackend backend = ml_api_.decide(sig);

        // 4. Execution Routing
        switch (backend) {
            case ExecutionBackend::JIT:
                execute_jit_path(cmd, sig);
                break;
            case ExecutionBackend::HAI:
                execute_hai_path(cmd, sig);
                break;
            case ExecutionBackend::Oracle:
            default:
                // Fallback to native driver code
                orig_draw_indexed_(cmd, indexCount, instanceCount, firstIndex, 0, 0);
                break;
        }
        const auto t1 = std::chrono::high_resolution_clock::now();

        // 5. Reward calculation & observe (non-blocking from render-thread perspective)
        const double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        power_estimator_.increment_frame();
        const auto pr = power_estimator_.generate();
        const uint32_t jit_over_budget = jit_stats_.is_over_budget() ? 1u : 0u;
        const float reward = static_cast<float>(reward_calc_.compute(static_cast<float>(elapsed_ms), pr, 0u, jit_over_budget));
        ml_api_.observe_from_signature(backend, reward, sig);
    }

    // -----------------------------------------------------------------------
    // JIT stutter telemetry – tracks Oracle fallback duration on cache miss
    // -----------------------------------------------------------------------
    struct JITStutterStats {
        uint32_t cold_cache_fallbacks = 0;   // Times JIT returned nullptr
        double   worst_fallback_ms    = 0.0; // Longest single Oracle fallback (ms)
        double   total_fallback_ms    = 0.0; // Cumulative time in Oracle fallback
        static constexpr double kBudgetMs = 2.0; // Acceptable stutter budget

        bool is_over_budget() const { return worst_fallback_ms > kBudgetMs; }
    };

private:
    void execute_jit_path(VkCommandBuffer cmd, const WorkloadSignature& sig) {
        // Use hash_utils.h canonical function (DRY fix: removed local duplicate)
        uint64_t ctx_hash = util::workload_context_hash(sig);
        auto specialized = jit_pipeline_.get_optimized_shader(sig.shader_hash, ctx_hash);

        if (specialized) {
            // Submit the specialized ISA directly to the GPU's Command Processor
            submit_isa_to_gpu(cmd, specialized->isa_binary);
        } else {
            // Cache miss: measure Oracle fallback duration to detect first-frame stutter
            const auto t0 = std::chrono::high_resolution_clock::now();
            orig_draw_indexed_(cmd, sig.vertex_count, 1, 0, 0, 0);
            const auto t1 = std::chrono::high_resolution_clock::now();

            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(t1 - t0).count();

            jit_stats_.cold_cache_fallbacks++;
            jit_stats_.total_fallback_ms += elapsed_ms;
            jit_stats_.worst_fallback_ms  =
                std::max(jit_stats_.worst_fallback_ms, elapsed_ms);
        }
    }

    void execute_hai_path(VkCommandBuffer cmd, const WorkloadSignature& sig) {
        // Build and stream the dense HAI bytecode
        hai_builder_.begin_batch();
        if (can_delta_update(sig)) {
            hai_builder_.write_delta_draw(sig);
        } else {
            hai_builder_.write_full_draw(sig);
        }
        hai_builder_.flush_to_hardware();
    }

    // calculate_context_hash removed — use synapse::util::workload_context_hash()
    // from hash_utils.h. See DRY audit in plan.md Phase 6.

    // Hardware interface stubs
    void submit_isa_to_gpu(VkCommandBuffer cmd, const std::vector<uint32_t>& isa) { /* MMIO Write */ }
    VkImage get_bound_image(VkCommandBuffer cmd) { /* State Tracking Logic */ return nullptr; }
    WorkloadSignature capture_current_signature(VkCommandBuffer cmd, uint32_t count) {
        WorkloadSignature sig{};
        sig.vertex_count = count;
        // In production, we'd query the bound PSO for the shader_hash here
        return sig;
    }

    // Original Dispatch Table
    PFN_vkCmdDrawIndexed orig_draw_indexed_;

    // JIT stutter telemetry (populated during execute_jit_path Oracle fallbacks)
    JITStutterStats        jit_stats_;

    // ML components
    synapse::ml::MLSubAPI        ml_api_;
    synapse::ml::RewardCalculator reward_calc_;
    synapse::metrics::PowerEstimator power_estimator_;

    // Synapse Sub-Modules
    TelemetryRingBuffer    telemetry_;
    Analyzer              analyzer_;
    Scheduler             scheduler_;
    JITPipeline           jit_pipeline_;
    HAIBytecodeBuilder    hai_builder_;
    TextureStreamingEngineHardened its_engine_;

    std::thread           analyzer_thread_;
    uint64_t              current_frame_ = 0;
};

} // namespace synapse