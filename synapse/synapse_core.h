// ============================================================================
// synapse/synapse_core.h
// Project Synapse – Unified Driver Integration Hub
// ============================================================================
#pragma once

#include "synapse_umd.h"
#include "synapse_jit_backend.h"
#include "synapse_hai_builder.h"
#include "its_engine_hardened.h"

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

    // ------------------------------------------------------------------------
    // PRODUCTION CRITICAL PATH: vkCmdDrawIndexed
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

        // 3. Backend Decision
        ExecutionBackend backend = scheduler_.decide_backend(sig);

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
    }

private:
    void execute_jit_path(VkCommandBuffer cmd, const WorkloadSignature& sig) {
        // Use the Lock-Free Specialization Cache we designed
        uint64_t ctx_hash = calculate_context_hash(sig);
        auto specialized = jit_pipeline_.get_optimized_shader(sig.shader_hash, ctx_hash);

        if (specialized) {
            // Submit the specialized ISA directly to the GPU's Command Processor
            submit_isa_to_gpu(cmd, specialized->isa_binary);
        } else {
            // Optimization not ready yet; use Oracle to avoid stalling the frame
            orig_draw_indexed_(cmd, sig.vertex_count, 1, 0, 0, 0);
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

    // Helper for contextual hashing: Context = ShaderID ^ WorkloadConditions
    uint64_t calculate_context_hash(const WorkloadSignature& sig) {
        // Using a standard hash_combine algorithm:
        // $$ H(s, w) = H(s) \oplus (H(w) + 0x9e3779b9 + (H(s) \ll 6) + (H(s) \gg 2)) $$
        uint64_t seed = sig.shader_hash;
        seed ^= sig.draw_call_count + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }

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