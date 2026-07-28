// ============================================================================\
// synapse/synapse_core.h
// Project Synapse – Unified Driver Integration Hub
// ============================================================================\
#pragma once

#include "synapse_umd.h"
#include "synapse_jit_backend.h"
#include "synapse_hai_builder.h"
#include "its_engine_hardened.h"
#include "hash_utils.h"     // canonical workload_context_hash — DRY fix
#include "telemetry_types.h"  // explicit: SynapseSessionReport used by build_session_report()
#include "ml/ml_sub_api.h"
#include "ml/reward_calculator.h"
#include "power_estimator.h"
#include <chrono>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

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
    SynapseCore(PFN_vkCmdDrawIndexed orig_draw,
                    PFN_vkCmdDraw orig_draw_non_indexed,
                    PFN_vkCmdDispatch orig_dispatch,
                    PFN_vkCmdPushConstants orig_push_constants,
                    PFN_vkCmdBindDescriptorSets orig_bind_desc_sets,
                    PFN_vkCmdBindShadersEXT orig_bind_shaders);
    ~SynapseCore();

    /// @brief Assembles a point-in-time session report from all live subsystems.
    ///        Used by AgentAPI::snapshot() and the CLI `snapshot` subcommand.
    synapse::telemetry::SynapseSessionReport build_session_report() const;

    /// @brief Returns JIT cold-cache stutter telemetry for reporting.
    const JITStutterStats& jit_stutter_stats() const { return jit_stats_; }

    /// @brief Wire the ITS engine's power estimator for bandwidth accounting.
    /// Called once after device creation.
    void wire_power_estimator(synapse::metrics::PowerEstimator* estimator) {
        its_engine_.set_power_estimator(estimator);
    }

    /// @brief Accessor for power estimator (used by layer_entry.cpp wiring).
    synapse::metrics::PowerEstimator* power_estimator() {
        return &power_estimator_;
    }

    /// @brief Record the most-recently-bound pipeline for @p cmd.
    /// @param shader_hash  FNV64 hash of the pipeline's shader stage path names.
    void notify_bind_pipeline(VkCommandBuffer cmd,
                                  VkPipeline      pipeline,
                                  uint64_t        shader_hash);

    /// @brief Record the primary sampled image for @p cmd (slot 0 of set 0).
    void notify_bind_image(VkCommandBuffer cmd, VkImage image);

    /// @brief Record a bound descriptor set for @p cmd — enables per-set texture tracking.
    void notify_bind_descriptor_set(VkCommandBuffer cmd, uint32_t set_index,
                                         const VkDescriptorSet& descriptor_set);

    /// @brief Register a texture image with the ITS engine when vkCreateImage is observed.
    void notify_create_image(VkImage image, const VkImageCreateInfo* create_info,
                                  uint64_t texture_id);

    /// @brief Unregister a texture image with the ITS engine when vkDestroyImage is observed.
    void notify_destroy_image(VkImage image);

    /// @brief Free tracking state when the command buffer is destroyed.
    void notify_free_cmd_buf(VkCommandBuffer cmd);

    /// @brief Called by Vulkan layer when vkCmdBindPipeline is intercepted.
    ///        Forwards to the per-cmd-buf state tracker.
    void notify_bind_pipeline(VkCommandBuffer cmd, VkPipeline pipeline) {
        notify_bind_pipeline(cmd, pipeline,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pipeline)));
    }

    // -------------------------------------------------------------------
    // Main interception entry points — called by layer_entry.cpp
    // All follow the same pattern: capture signature, route to backend.
    // -------------------------------------------------------------------

    /// @brief Intercept vkCmdDrawIndexed — route to JIT / HAI / Oracle.
    void handle_draw_indexed(
        VkCommandBuffer cmd,
        uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndex,
        int32_t  vertexOffset = 0,
        uint32_t firstInstance = 0);

    /// @brief Intercept vkCmdDraw — non-indexed variant.
    void handle_draw(
        VkCommandBuffer cmd,
        uint32_t vertexCount,
        uint32_t instanceCount,
        uint32_t firstVertex,
        uint32_t firstInstance);

    /// @brief Intercept vkCmdDispatch — compute dispatch variant.
    void handle_dispatch(
        VkCommandBuffer cmd,
        uint32_t groupCountX,
        uint32_t groupCountY,
        uint32_t groupCountZ);

    /// @brief Intercept vkCmdPushConstants — feeds the PushConstantOptimizer.
    void handle_push_constants(
        VkCommandBuffer cmd,
        VkPipelineLayout layout,
        uint32_t        offset,
        uint32_t        size,
        const void*     pValues);

    /// @brief Intercept vkCmdBindDescriptorSets — enables per-set texture tracking.
    void handle_bind_descriptor_sets(
        VkCommandBuffer                    cmd,
        VkPipelineBindPoint                bindPoint,
        VkPipelineLayout                   layout,
        uint32_t                           firstSet,
        uint32_t                           descriptorSetCount,
        const VkDescriptorSet*             pDescriptorSets,
        uint32_t                           dynamicOffsetCount,
        const uint32_t*                    pDynamicOffsets);

    // ----------------------------------------------------------------------
    // JIT stutter telemetry – tracks Oracle fallback duration on cache miss
    // ----------------------------------------------------------------------
    struct JITStutterStats {
        uint32_t cold_cache_fallbacks = 0;   // Times JIT returned nullptr
        double   worst_fallback_ms    = 0.0; // Longest single Oracle fallback (ms)
        double   total_fallback_ms    = 0.0; // Cumulative time in Oracle fallback
        static constexpr double kBudgetMs = 2.0; // Acceptable stutter budget

        bool is_over_budget() const { return worst_fallback_ms > kBudgetMs; }
    };

private:
    // ------------------------------------------------------------------
    // Backends
    // ------------------------------------------------------------------
    void execute_jit_path(VkCommandBuffer cmd, const WorkloadSignature& sig);
    void execute_hai_path(VkCommandBuffer cmd, const WorkloadSignature& sig);
    void execute_oracle_draw(VkCommandBuffer cmd, const WorkloadSignature& sig,
                                 uint32_t indexCount, uint32_t instanceCount,
                                 uint32_t firstIndex, int32_t vertexOffset,
                                 uint32_t firstInstance);

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------
    bool can_delta_update(const WorkloadSignature& sig) const;
    VkImage get_bound_image(VkCommandBuffer cmd);
    WorkloadSignature capture_current_signature(VkCommandBuffer cmd, uint32_t count);
    void increment_backend_counter(ExecutionBackend backend);

    // ------------------------------------------------------------------
    // Hardware interface stubs — T1-3/T1-4/T1-5
    // ------------------------------------------------------------------
    void submit_isa_to_gpu(VkCommandBuffer /*cmd*/,
                                const std::vector<uint32_t>& /*isa*/);

    // ------------------------------------------------------------------
    // Per-command-buffer pipeline/image state
    // ------------------------------------------------------------------
    struct CmdBufState {
        VkPipeline              bound_pipeline = VK_NULL_HANDLE;
        VkImage                 bound_image    = VK_NULL_HANDLE; ///< primary sampled slot (set 0, binding 0)
        uint64_t                shader_hash    = 0;              ///< FNV64 of pipeline shader paths
        std::array<VkDescriptorSet, 4> descriptor_sets{};       ///< bound descriptor sets
    };

    // Original Dispatch Table
    PFN_vkCmdDrawIndexed orig_draw_indexed_;
    PFN_vkCmdDraw        orig_draw_         = nullptr;
    PFN_vkCmdDispatch    orig_dispatch_     = nullptr;
    PFN_vkCmdPushConstants orig_push_constants_ = nullptr;
    PFN_vkCmdBindDescriptorSets orig_bind_descriptor_sets_ = nullptr;
    PFN_vkCmdBindShadersEXT    orig_bind_shaders_       = nullptr;

    // JIT stutter telemetry
    JITStutterStats jit_stats_;

    // Backend routing counters for report.json
    uint32_t backend_jit_count_    = 0;
    uint32_t backend_hai_count_    = 0;
    uint32_t backend_oracle_count_ = 0;

    // Per-command-buffer pipeline/image state
    std::unordered_map<VkCommandBuffer, CmdBufState> cmd_state_;
    std::shared_mutex cmd_state_mutex_;

    // ML components
    synapse::ml::MLSubAPI          ml_api_;
    synapse::ml::RewardCalculator  reward_calc_;
    synapse::metrics::PowerEstimator power_estimator_;

    // Synapse Sub-Modules
    TelemetryRingBuffer             telemetry_;
    Analyzer                        analyzer_;
    Scheduler                       scheduler_;
    JITPipeline                     jit_pipeline_;
    HAIBytecodeBuilder              hai_builder_;
    TextureStreamingEngineHardened  its_engine_;

    std::thread  analyzer_thread_;
    uint64_t     current_frame_ = 0;
};

} // namespace synapse