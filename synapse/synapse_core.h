// ============================================================================
// synapse/synapse_core.h
// Project Synapse – Unified Driver Integration Hub
//
// Atomic operations, crash-safe telemetry, graceful degradation,
// hot-reload, user profiling, and schema versioning wired in.
// ============================================================================
#pragma once

#include "synapse_umd.h"
#include "synapse_jit_backend.h"
#include "synapse_hai_builder.h"
#include "its_engine_hardened.h"
#include "hash_utils.h"
#include "telemetry_types.h"
#include "ml/ml_sub_api.h"
#include "ml/reward_calculator.h"
#include "power_estimator.h"

// Production modules
#include "atomic/atomic_state.h"
#include "atomic/atomic_config.h"
#include "atomic/atomic_telemetry.h"
#include "atomic/graceful_degradation.h"
#include "recovery/crash_recovery.h"
#include "personal/user_profile.h"
#include "hotreload/config_watcher.h"
#include "protocol/schema_migration.h"

#include <chrono>
#include <algorithm>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace synapse {

/**
 * @class SynapseCore
 * @brief The central coordinator for Project Synapse.
 *
 * Production features:
 *   - AtomicStateMachine for thread-safe state transitions
 *   - AtomicTelemetry with WAL for crash-safe logging
 *   - GracefulDegradation for automatic feature fallback
 *   - AtomicConfig for hot-reloadable configuration
 *   - UserProfile for personal-use optimization
 *   - ConfigWatcher for live config file monitoring
 *   - SchemaMigration for versioned data formats
 */
class SynapseCore {
public:
    // JIT stutter telemetry
    struct JITStutterStats {
        uint32_t cold_cache_fallbacks = 0;
        double   worst_fallback_ms    = 0.0;
        double   total_fallback_ms    = 0.0;
        static constexpr double kBudgetMs = 2.0;
        bool is_over_budget() const { return worst_fallback_ms > kBudgetMs; }
    };

    // Construct with data directory for WAL/recovery
    explicit SynapseCore(
        PFN_vkCmdDrawIndexed orig_draw,
        PFN_vkCmdDraw orig_draw_non_indexed,
        PFN_vkCmdDispatch orig_dispatch,
        PFN_vkCmdPushConstants orig_push_constants,
        PFN_vkCmdBindDescriptorSets orig_bind_desc_sets,
        PFN_vkCmdBindShadersEXT orig_bind_shaders,
        const std::string& data_dir = ".");

    ~SynapseCore();

    // ------------------------------------------------------------------
    // Recovery
    // ------------------------------------------------------------------
    struct RecoveryInfo {
        bool crash_detected{false};
        uint64_t entries_recovered{0};
        uint64_t recovery_count{0};
    };
    RecoveryInfo check_and_recover();

    // ------------------------------------------------------------------
    // Session report
    // ------------------------------------------------------------------
    synapse::telemetry::SynapseSessionReport build_session_report() const;
    const JITStutterStats& jit_stutter_stats() const { return jit_stats_; }

    // ------------------------------------------------------------------
    // Power estimator
    // ------------------------------------------------------------------
    void wire_power_estimator(synapse::metrics::PowerEstimator* estimator) {
        its_engine_.set_power_estimator(estimator);
    }
    synapse::metrics::PowerEstimator* power_estimator() { return &power_estimator_; }

    // ------------------------------------------------------------------
    // Production module accessors
    // ------------------------------------------------------------------
    synapse::atomic::AtomicStateMachine& state_machine()    { return state_; }
    synapse::atomic::AtomicConfig&       config()           { return config_; }
    synapse::atomic::GracefulDegradation& degradation()     { return degrade_; }
    synapse::personal::UserProfile&       user_profile()    { return user_profile_; }
    recovery::CrashRecoveryManager*       recovery()        { return recovery_.get(); }
    synapse::hotreload::ConfigWatcher*    config_watcher()  { return config_watcher_.get(); }

    // Feature availability check (degradation-aware)
    bool is_feature_available(const std::string& feature) const {
        return degrade_.is_available(feature);
    }

    ExecutionBackend current_analyzer_recommendation() const {
        return analyzer_.current_recommendation();
    }

    // ------------------------------------------------------------------
    // Config synchronization: UserProfile → AtomicConfig
    // Call after apply_preset() or update_usage() to propagate to config_.
    // ------------------------------------------------------------------
    void sync_config_from_profile();

    // ------------------------------------------------------------------
    // Runtime profile switching (applies preset + syncs config)
    // ------------------------------------------------------------------
    void apply_preset(const char* name);

    // ------------------------------------------------------------------
    // Data directory accessor (for profile persistence)
    // ------------------------------------------------------------------
    const std::string& data_dir() const { return data_dir_; }

    // ------------------------------------------------------------------
    // Notification hooks (layer_entry.cpp calls these)
    // ------------------------------------------------------------------
    void notify_bind_pipeline(VkCommandBuffer cmd, VkPipeline pipeline,
                                  uint64_t shader_hash);
    void notify_bind_image(VkCommandBuffer cmd, VkImage image);
    void notify_bind_descriptor_set(VkCommandBuffer cmd, uint32_t set_index,
                                         const VkDescriptorSet& descriptor_set);
    void notify_create_image(VkImage image, const VkImageCreateInfo* create_info,
                                  uint64_t texture_id);
    void notify_destroy_image(VkImage image);
    void notify_free_cmd_buf(VkCommandBuffer cmd);
    void notify_bind_pipeline(VkCommandBuffer cmd, VkPipeline pipeline) {
        notify_bind_pipeline(cmd, pipeline,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pipeline)));
    }

    // ------------------------------------------------------------------
    // Main interception entry points
    // ------------------------------------------------------------------
    void handle_draw_indexed(
        VkCommandBuffer cmd, uint32_t indexCount, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset = 0, uint32_t firstInstance = 0);

    void handle_draw(
        VkCommandBuffer cmd, uint32_t vertexCount, uint32_t instanceCount,
        uint32_t firstVertex, uint32_t firstInstance);

    void handle_dispatch(
        VkCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY,
        uint32_t groupCountZ);

    void handle_push_constants(
        VkCommandBuffer cmd, VkPipelineLayout layout,
        VkShaderStageFlags stageFlags, uint32_t offset,
        uint32_t size, const void* pValues);

    void handle_bind_descriptor_sets(
        VkCommandBuffer cmd, VkPipelineBindPoint bindPoint,
        VkPipelineLayout layout, uint32_t firstSet,
        uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets,
        uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets);

private:
    // Backend execution
    void execute_jit_path(VkCommandBuffer cmd, const WorkloadSignature& sig);
    void execute_hai_path(VkCommandBuffer cmd, const WorkloadSignature& sig);
    void execute_oracle_draw(VkCommandBuffer cmd, const WorkloadSignature& sig,
                                 uint32_t indexCount, uint32_t instanceCount,
                                 uint32_t firstIndex, int32_t vertexOffset,
                                 uint32_t firstInstance);

    // Helpers
    bool can_delta_update(const WorkloadSignature& sig) const;
    VkImage get_bound_image(VkCommandBuffer cmd);
    WorkloadSignature capture_current_signature(VkCommandBuffer cmd, uint32_t count);
    void increment_backend_counter(ExecutionBackend backend);
    void submit_isa_to_gpu(VkCommandBuffer cmd, const std::vector<uint32_t>& isa);

    // WAL write helper — logs event to crash-safe telemetry
    void wal_log(atomic::WALEventType type, const void* data = nullptr, uint32_t size = 0);

    // Start config file watcher (called from constructor)
    void start_config_watcher();

    // Optional D3D12 helper-DLL attachment (Windows-only, best-effort).
    bool try_attach_d3d12_helper();

    // Parse a TOML-like config file content and apply to config_
    void apply_config_content(const std::string& content);

    // ------------------------------------------------------------------
    // Per-command-buffer state
    // ------------------------------------------------------------------
    struct CmdBufState {
        VkPipeline              bound_pipeline = VK_NULL_HANDLE;
        VkImage                 bound_image    = VK_NULL_HANDLE;
        uint64_t                shader_hash    = 0;
        std::array<VkDescriptorSet, 4> descriptor_sets{};
    };

    // Original dispatch table
    PFN_vkCmdDrawIndexed orig_draw_indexed_;
    PFN_vkCmdDraw        orig_draw_         = nullptr;
    PFN_vkCmdDispatch    orig_dispatch_     = nullptr;
    PFN_vkCmdPushConstants orig_push_constants_ = nullptr;
    PFN_vkCmdBindDescriptorSets orig_bind_descriptor_sets_ = nullptr;
    PFN_vkCmdBindShadersEXT    orig_bind_shaders_       = nullptr;

    // JIT stutter telemetry
    JITStutterStats jit_stats_;

    // Backend routing counters
    uint32_t backend_jit_count_    = 0;
    uint32_t backend_hai_count_    = 0;
    uint32_t backend_oracle_count_ = 0;

    // Per-command-buffer state
    std::unordered_map<VkCommandBuffer, CmdBufState> cmd_state_;
    std::shared_mutex cmd_state_mutex_;

    // ML components
    synapse::ml::MLSubAPI          ml_api_;
    synapse::ml::RewardCalculator  reward_calc_;
    synapse::metrics::PowerEstimator power_estimator_;

    // Synapse sub-modules
    TelemetryRingBuffer             telemetry_;
    Analyzer                        analyzer_;
    Scheduler                       scheduler_;
    JITPipeline                     jit_pipeline_;
    builder::HAIBytecodeBuilder     hai_builder_;
    TextureStreamingEngineHardened  its_engine_;

    std::thread  analyzer_thread_;
    uint64_t     current_frame_ = 0;

    // ── Production modules ──────────────────────────────────────────
    synapse::atomic::AtomicStateMachine    state_;
    synapse::atomic::AtomicConfig          config_;
    synapse::atomic::GracefulDegradation   degrade_;
    synapse::personal::UserProfile         user_profile_;
    std::unique_ptr<recovery::CrashRecoveryManager> recovery_;
    std::unique_ptr<hotreload::ConfigWatcher> config_watcher_;
    std::string data_dir_;
};

} // namespace synapse
