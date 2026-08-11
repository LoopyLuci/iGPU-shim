// ============================================================================
// synapse/synapse_core.cpp
// Project Synapse – Core routing with Atomic, WAL, Degradation, Config,
//                   User Profile, ConfigWatcher, and Crash Recovery wired in.
// ============================================================================
#include "synapse_core.h"

#include "hai_frontend_sim.h"

#if defined(_WIN32)
# include <windows.h>
#endif

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace synapse {

// ===========================================================================
// Construction / Destruction
// ===========================================================================
SynapseCore::SynapseCore(
    PFN_vkCmdDrawIndexed orig_draw,
    PFN_vkCmdDraw orig_draw_non_indexed,
    PFN_vkCmdDispatch orig_dispatch,
    PFN_vkCmdPushConstants orig_push_constants,
    PFN_vkCmdBindDescriptorSets orig_bind_desc_sets,
    PFN_vkCmdBindShadersEXT orig_bind_shaders,
    const std::string& data_dir)
    : orig_draw_indexed_(orig_draw),
      orig_draw_(orig_draw_non_indexed),
      orig_dispatch_(orig_dispatch),
      orig_push_constants_(orig_push_constants),
      orig_bind_descriptor_sets_(orig_bind_desc_sets),
      orig_bind_shaders_(orig_bind_shaders),
      analyzer_(telemetry_),
      scheduler_(analyzer_),
      jit_pipeline_(analyzer_),
      its_engine_(analyzer_),
      state_(atomic::ShimState::Uninitialized),
      config_(),
      degrade_(),
      user_profile_(),
      data_dir_(data_dir)
{
    // Ensure data directory exists
    std::error_code ec;
    std::filesystem::create_directories(data_dir_, ec);

    // Initialize crash recovery manager (owns the WAL)
    recovery_ = std::make_unique<recovery::CrashRecoveryManager>(data_dir_);

    // Transition: Uninitialized → Initializing
    state_.transition(atomic::ShimState::Initializing, "SynapseCore construction");

    // Register feature flags
    degrade_.register_feature("jit", atomic::FeatureState::Enabled);
    degrade_.register_feature("hai", atomic::FeatureState::Enabled);
    degrade_.register_feature("its", atomic::FeatureState::Enabled);
    degrade_.register_feature("ml", atomic::FeatureState::Enabled);
    degrade_.register_feature("telemetry", atomic::FeatureState::Enabled);
    degrade_.register_feature("power", atomic::FeatureState::Enabled);

    // Apply user profile preset to config_ on startup
    sync_config_from_profile();

    // Start analyzer thread
    analyzer_thread_ = std::thread(&Analyzer::process_telemetry_loop, &analyzer_);

    // Transition: Initializing → Active
    state_.transition(atomic::ShimState::Active, "Initialization complete");

    // Start config file watcher for live reload
    start_config_watcher();

#if defined(_WIN32)
    if (!try_attach_d3d12_helper()) {
        // Helper DLL not present or failed to attach; layer continues without D3D12 interception.
    }
#endif
}

SynapseCore::~SynapseCore() {
    // Stop config watcher first
    if (config_watcher_) {
        config_watcher_->stop();
    }

    // Transition to ShuttingDown
    state_.transition(atomic::ShimState::ShuttingDown, "Destructor");

    // Mark clean shutdown via WAL (through CrashRecoveryManager)
    // Skip if crash was simulated (simulate_crash sets clean_shutdown_ = true)
    if (recovery_ && !recovery_->telemetry().is_clean_shutdown()) {
        recovery_->mark_clean_shutdown();
    }

    // Stop analyzer thread
    analyzer_.shutdown();
    if (analyzer_thread_.joinable()) analyzer_thread_.join();

    // Transition to Uninitialized
    state_.transition(atomic::ShimState::Uninitialized, "Destructor complete");
}

// ===========================================================================
// Config synchronization: UserProfile → AtomicConfig
// ===========================================================================
void SynapseCore::sync_config_from_profile() {
    const auto& plan = user_profile_.plan();
    config_.update([&plan](atomic::ConfigSnapshot& snap) {
        snap.power_budget_watts    = plan.power_budget_watts;
        snap.thermal_target_celsius = plan.thermal_target_celsius;
        snap.ml_aggressive         = plan.ml_aggressive;
    });

    // Also sync feature enable/disable based on config
    degrade_.handle_error("jit",     config_.jit_enabled()     ? 0 : 2);
    degrade_.handle_error("hai",     config_.hai_enabled()     ? 0 : 2);
    degrade_.handle_error("telemetry", config_.telemetry_enabled() ? 0 : 2);
}

// ===========================================================================
// Runtime profile switching: apply preset → sync config
// ===========================================================================
void SynapseCore::apply_preset(const char* name) {
    user_profile_.apply_preset(name);
    sync_config_from_profile();
    wal_log(atomic::WALEventType::BackendChoice, name,
            static_cast<uint32_t>(std::strlen(name)));
}

// ===========================================================================
// Config file watcher — watches data_dir_/config.toml for live reload
// ===========================================================================
void SynapseCore::start_config_watcher() {
    auto config_path = std::filesystem::path(data_dir_) / "config.toml";

    // Create a default config file if none exists
    if (!std::filesystem::exists(config_path)) {
        std::ofstream ofs(config_path);
        if (ofs) {
            ofs << "# Synapse configuration\n";
            ofs << "# Edit while the layer is running — changes take effect immediately.\n";
            ofs << "power_budget = " << config_.power_budget() << "\n";
            ofs << "thermal_target = " << config_.thermal_target() << "\n";
            ofs << "ml_aggressive = " << (config_.ml_aggressive() ? "true" : "false") << "\n";
            ofs << "jit_enabled = true\n";
            ofs << "hai_enabled = true\n";
            ofs << "telemetry_enabled = true\n";
        }
    }

    config_watcher_ = std::make_unique<hotreload::ConfigWatcher>(config_path);
    config_watcher_->on_change([this](const std::string& content) {
        apply_config_content(content);
    });
    config_watcher_->start();
}

#if defined(_WIN32)
bool SynapseCore::try_attach_d3d12_helper() {
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path = exePath;
    const auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path = path.substr(0, pos + 1);
    path += "SynapseD3D12Helper.dll";

    HMODULE module = LoadLibraryA(path.c_str());
    if (!module) {
        return false;
    }

    auto attach = reinterpret_cast<long (__stdcall*)()>(
        GetProcAddress(module, "attach_process_hooks"));
    if (attach) {
        attach();
    }

    // Keep module loaded for the lifetime of SynapseCore.
    // `detach_process_hooks` will be called from the destructor path
    // if/when explicit teardown is added later.
    return true;
}
#endif

// ===========================================================================
// Parse TOML-like config content and apply to AtomicConfig + Degradation
// ===========================================================================
void SynapseCore::apply_config_content(const std::string& content) {
    std::istringstream iss(content);
    std::string line;

    // Read current values
    auto snap = config_.read();

    while (std::getline(iss, line)) {
        // Skip comments and blank lines
        if (line.empty() || line[0] == '#') continue;

        // Trim whitespace
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(line);

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trim(key);
        trim(val);

        // Parse known keys
        if (key == "power_budget") {
            auto v = static_cast<uint32_t>(std::stoul(val));
            if (v > 0 && v <= 100) snap.power_budget_watts = v;
        } else if (key == "thermal_target") {
            auto v = static_cast<uint32_t>(std::stoul(val));
            if (v >= 40 && v <= 105) snap.thermal_target_celsius = v;
        } else if (key == "ml_aggressive") {
            snap.ml_aggressive = (val == "true" || val == "1");
        } else if (key == "jit_enabled") {
            snap.jit_enabled = (val == "true" || val == "1");
        } else if (key == "hai_enabled") {
            snap.hai_enabled = (val == "true" || val == "1");
        } else if (key == "telemetry_enabled") {
            snap.telemetry_enabled = (val == "true" || val == "1");
        } else if (key == "learning_rate") {
            auto v = std::stof(val);
            if (v > 0.0f && v <= 1.0f) snap.learning_rate = v;
        } else if (key == "epsilon") {
            auto v = std::stof(val);
            if (v >= 0.0f && v <= 1.0f) snap.epsilon = v;
        }
    }

    // Atomic update
    config_.update([&snap](atomic::ConfigSnapshot& dest) {
        dest = snap;
    });

    // Propagate feature flags to degradation
    degrade_.handle_error("jit",       snap.jit_enabled       ? 0 : 2);
    degrade_.handle_error("hai",       snap.hai_enabled       ? 0 : 2);
    degrade_.handle_error("telemetry", snap.telemetry_enabled ? 0 : 2);
}

// ===========================================================================
// Crash Recovery
// ===========================================================================
SynapseCore::RecoveryInfo SynapseCore::check_and_recover() {
    RecoveryInfo info;

    if (!recovery_) return info;

    info.crash_detected = recovery_->check_for_crash();
    if (info.crash_detected) {
        // Degrade telemetry during recovery
        degrade_.handle_error("telemetry", 1);  // Transient → degraded

        auto result = recovery_->recover();
        info.entries_recovered = result.entries_recovered;
        info.recovery_count = recovery_->metadata().total_recoveries;

        // Restore telemetry after recovery
        degrade_.handle_error("telemetry", 0);  // Recovery
    }

    return info;
}

// ===========================================================================
// WAL logging helper
// ===========================================================================
void SynapseCore::wal_log(atomic::WALEventType type, const void* data, uint32_t size) {
    if (!recovery_) return;
    if (!degrade_.is_available("telemetry")) return;
    recovery_->telemetry().write(type, data, size);
}

// ===========================================================================
// Session report
// ===========================================================================
synapse::telemetry::SynapseSessionReport SynapseCore::build_session_report() const {
    synapse::telemetry::SynapseSessionReport r{};

    r.jit.cold_cache_fallbacks = jit_stats_.cold_cache_fallbacks;
    r.jit.worst_fallback_ms    = jit_stats_.worst_fallback_ms;
    r.jit.total_fallback_ms    = jit_stats_.total_fallback_ms;

    r.its_cache.hits   = its_engine_.get_hits();
    r.its_cache.misses = its_engine_.get_misses();

    r.backend_routing.jit_dispatches    = backend_jit_count_;
    r.backend_routing.hai_dispatches    = backend_hai_count_;
    r.backend_routing.oracle_dispatches = backend_oracle_count_;
    r.backend_routing.total_draw_calls  =
        backend_jit_count_ + backend_hai_count_ + backend_oracle_count_;

    r.ml_model.total_updates     = ml_api_.updates();
    r.ml_model.cumulative_reward = static_cast<double>(ml_api_.cumulative_reward());

    r.power = power_estimator_.generate();

    return r;
}

// ===========================================================================
// Notification hooks
// ===========================================================================
void SynapseCore::notify_bind_pipeline(VkCommandBuffer cmd,
                                                VkPipeline      pipeline,
                                                uint64_t        shader_hash) {
    std::unique_lock<std::shared_mutex> lock(cmd_state_mutex_);
    auto& s = cmd_state_[cmd];
    s.bound_pipeline = pipeline;
    s.shader_hash    = shader_hash;
}

void SynapseCore::notify_bind_image(VkCommandBuffer cmd, VkImage image) {
    std::unique_lock<std::shared_mutex> lock(cmd_state_mutex_);
    cmd_state_[cmd].bound_image = image;
}

void SynapseCore::notify_bind_descriptor_set(VkCommandBuffer cmd, uint32_t set_index,
                                                   const VkDescriptorSet& descriptor_set) {
    std::unique_lock<std::shared_mutex> lock(cmd_state_mutex_);
    auto& s = cmd_state_[cmd];
    if (set_index < s.descriptor_sets.size()) {
        s.descriptor_sets[set_index] = descriptor_set;
    }
}

void SynapseCore::notify_create_image(VkImage image, const VkImageCreateInfo* create_info,
                                             uint64_t texture_id) {
    its_engine_.register_texture(image, create_info, texture_id);
}

void SynapseCore::notify_destroy_image(VkImage image) {
    its_engine_.unregister_texture(image);
}

void SynapseCore::notify_free_cmd_buf(VkCommandBuffer cmd) {
    std::unique_lock<std::shared_mutex> lock(cmd_state_mutex_);
    cmd_state_.erase(cmd);
}

// ===========================================================================
// Vulkan command interception — config-driven, WAL-logged, degradation-aware
// ===========================================================================
void SynapseCore::handle_draw_indexed(
    VkCommandBuffer cmd,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t  vertexOffset,
    uint32_t firstInstance)
{
    // Check if JIT feature is available
    if (!degrade_.is_available("jit")) {
        // Degrade: use Oracle directly
        if (orig_draw_indexed_) {
            orig_draw_indexed_(cmd, indexCount, instanceCount,
                               firstIndex, vertexOffset, firstInstance);
        }
        wal_log(atomic::WALEventType::DrawIndexed);
        return;
    }

    WorkloadSignature sig = capture_current_signature(cmd, indexCount);

    // Log to WAL (crash-safe)
    wal_log(atomic::WALEventType::DrawIndexed, &sig, sizeof(sig));

    telemetry_.push(sig);
    analyzer_.update_its_stats(its_engine_.get_hits(), its_engine_.get_misses());
    its_engine_.prepare_for_use(get_bound_image(cmd), current_frame_);

    const auto t0 = std::chrono::high_resolution_clock::now();

    // Read config for ML decision influence
    const auto cfg = config_.read();

    synapse::telemetry::SynapseSessionReport live_report{};
    live_report.jit.cold_cache_fallbacks = jit_stats_.cold_cache_fallbacks;
    live_report.jit.worst_fallback_ms    = jit_stats_.worst_fallback_ms;
    live_report.jit.total_fallback_ms    = jit_stats_.total_fallback_ms;
    live_report.its_cache.hits   = its_engine_.get_hits();
    live_report.its_cache.misses = its_engine_.get_misses();
    live_report.its_cache.current_usage_bytes = 0;
    ExecutionBackend backend = ml_api_.decide(sig, &live_report);

    // Route to backend with degradation checks
    increment_backend_counter(backend);
    switch (backend) {
        case ExecutionBackend::JIT:
            if (degrade_.is_available("jit")) {
                execute_jit_path(cmd, sig);
            } else {
                execute_oracle_draw(cmd, sig, indexCount, instanceCount,
                                    firstIndex, vertexOffset, firstInstance);
            }
            break;
        case ExecutionBackend::HAI:
            if (degrade_.is_available("hai")) {
                execute_hai_path(cmd, sig);
            } else {
                execute_oracle_draw(cmd, sig, indexCount, instanceCount,
                                    firstIndex, vertexOffset, firstInstance);
            }
            break;
        case ExecutionBackend::Oracle:
        default:
            execute_oracle_draw(cmd, sig, indexCount, instanceCount,
                                firstIndex, vertexOffset, firstInstance);
            break;
    }

    const auto t1 = std::chrono::high_resolution_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    power_estimator_.increment_frame();
    const auto pr = power_estimator_.generate();
    live_report.power = pr;

    // Config-driven reward: if over thermal target, penalize
    const uint32_t jit_over_budget = jit_stats_.is_over_budget() ? 1u : 0u;
    float reward = static_cast<float>(
        reward_calc_.compute(static_cast<float>(elapsed_ms), pr, 0u, jit_over_budget));

    // Config: if ml_aggressive is off, dampen reward signal
    if (!cfg.ml_aggressive) {
        reward *= 0.5f;
    }

    ml_api_.observe_from_signature(backend, reward, sig, &live_report);
}

void SynapseCore::handle_draw(
    VkCommandBuffer cmd,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance)
{
    WorkloadSignature sig = capture_current_signature(cmd, vertexCount);
    sig.is_compute_dispatch = false;

    wal_log(atomic::WALEventType::Draw);
    telemetry_.push(sig);
    analyzer_.update_its_stats(its_engine_.get_hits(), its_engine_.get_misses());
    its_engine_.prepare_for_use(get_bound_image(cmd), current_frame_);

    ExecutionBackend backend = ml_api_.decide(sig, nullptr);
    increment_backend_counter(backend);

    switch (backend) {
        case ExecutionBackend::JIT:
            if (degrade_.is_available("jit")) {
                execute_jit_path(cmd, sig);
            } else if (orig_draw_) {
                orig_draw_(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
            }
            break;
        case ExecutionBackend::HAI:
            if (degrade_.is_available("hai")) {
                execute_hai_path(cmd, sig);
            } else if (orig_draw_) {
                orig_draw_(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
            }
            break;
        case ExecutionBackend::Oracle:
        default:
            if (orig_draw_) {
                orig_draw_(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
            }
            break;
    }
}

void SynapseCore::handle_dispatch(
    VkCommandBuffer cmd,
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ)
{
    // Read config for degradation decisions
    const auto cfg = config_.read();

    WorkloadSignature sig{};
    sig.is_compute_dispatch     = true;
    sig.draw_call_count         = 1;
    sig.shader_instruction_estimate = 500;

    wal_log(atomic::WALEventType::Dispatch, &sig, sizeof(sig));
    telemetry_.push(sig);

    ExecutionBackend backend = ml_api_.decide(sig, nullptr);
    increment_backend_counter(backend);

    switch (backend) {
        case ExecutionBackend::JIT:
            if (degrade_.is_available("jit") && cfg.jit_enabled) {
                execute_jit_path(cmd, sig);
            } else if (orig_dispatch_) {
                orig_dispatch_(cmd, groupCountX, groupCountY, groupCountZ);
            }
            break;
        case ExecutionBackend::HAI:
            if (degrade_.is_available("hai") && cfg.hai_enabled) {
                execute_hai_path(cmd, sig);
            } else if (orig_dispatch_) {
                orig_dispatch_(cmd, groupCountX, groupCountY, groupCountZ);
            }
            break;
        case ExecutionBackend::Oracle:
        default:
            if (orig_dispatch_) {
                orig_dispatch_(cmd, groupCountX, groupCountY, groupCountZ);
            }
            break;
    }

    // Config: if ml_aggressive off, skip reward observation
    if (cfg.ml_aggressive) {
        const auto t1 = std::chrono::high_resolution_clock::now();
        // Approximate elapsed from WAL write timing
        const auto pr = power_estimator_.generate();
        const uint32_t jit_over_budget = jit_stats_.is_over_budget() ? 1u : 0u;
        float reward = static_cast<float>(
            reward_calc_.compute(0.5f, pr, 0u, jit_over_budget));
        synapse::telemetry::SynapseSessionReport live_report{};
        live_report.power = pr;
        ml_api_.observe_from_signature(backend, reward, sig, &live_report);
    }
}

void SynapseCore::handle_push_constants(
    VkCommandBuffer cmd,
    VkPipelineLayout layout,
    VkShaderStageFlags stageFlags,
    uint32_t        offset,
    uint32_t        size,
    const void*     pValues)
{
    wal_log(atomic::WALEventType::PushConstants, pValues, (std::min)(size, 240u));
    if (orig_push_constants_) {
        orig_push_constants_(cmd, layout, stageFlags, offset, size, pValues);
    }
}

void SynapseCore::handle_bind_descriptor_sets(
    VkCommandBuffer                    cmd,
    VkPipelineBindPoint                bindPoint,
    VkPipelineLayout                   layout,
    uint32_t                           firstSet,
    uint32_t                           descriptorSetCount,
    const VkDescriptorSet*             pDescriptorSets,
    uint32_t                           dynamicOffsetCount,
    const uint32_t*                    pDynamicOffsets)
{
    for (uint32_t i = 0; i < descriptorSetCount; ++i) {
        notify_bind_descriptor_set(cmd, firstSet + i, pDescriptorSets[i]);
    }
    wal_log(atomic::WALEventType::BindDescriptorSets);
    if (orig_bind_descriptor_sets_) {
        orig_bind_descriptor_sets_(cmd, bindPoint, layout, firstSet,
                                       descriptorSetCount, pDescriptorSets,
                                       dynamicOffsetCount, pDynamicOffsets);
    }
}

// ===========================================================================
// Backend execution helpers
// ===========================================================================
void SynapseCore::execute_oracle_draw(VkCommandBuffer cmd,
                                               const WorkloadSignature& sig,
                                               uint32_t indexCount,
                                               uint32_t instanceCount,
                                               uint32_t firstIndex,
                                               int32_t vertexOffset,
                                               uint32_t firstInstance) {
    const auto t0 = std::chrono::high_resolution_clock::now();
    orig_draw_indexed_(cmd, indexCount, instanceCount, firstIndex,
                         vertexOffset, firstInstance);
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    jit_stats_.cold_cache_fallbacks++;
    jit_stats_.total_fallback_ms += elapsed_ms;
    jit_stats_.worst_fallback_ms  =
        (std::max)(jit_stats_.worst_fallback_ms, elapsed_ms);
}

void SynapseCore::increment_backend_counter(ExecutionBackend backend) {
    switch (backend) {
        case ExecutionBackend::JIT:     backend_jit_count_++;     break;
        case ExecutionBackend::HAI:     backend_hai_count_++;     break;
        case ExecutionBackend::Oracle:  backend_oracle_count_++;  break;
    }
}

// ===========================================================================
// Private helpers
// ===========================================================================
void SynapseCore::execute_jit_path(VkCommandBuffer cmd,
                                                   const WorkloadSignature& sig) {
    uint64_t ctx_hash = util::workload_context_hash(sig);
    auto specialized = jit_pipeline_.get_optimized_shader(sig.shader_hash, std::vector<uint32_t>{});
    if (specialized) {
        submit_isa_to_gpu(cmd, specialized->isa_binary);
    } else {
        // JIT cache miss → degrade JIT feature
        degrade_.handle_error("jit", 1);
        execute_oracle_draw(cmd, sig, sig.vertex_count, 1, 0, 0, 0);
        // Restore JIT after one frame
        degrade_.handle_error("jit", 0);
    }
}

void SynapseCore::execute_hai_path(VkCommandBuffer cmd,
                                                   const WorkloadSignature& sig) {
    hai_builder_.begin_batch();
    if (can_delta_update(sig)) {
        hai_builder_.write_delta_draw(sig);
    } else {
        hai_builder_.write_full_draw(sig);
    }
    hai_builder_.flush_to_hardware();
}

bool SynapseCore::can_delta_update(const WorkloadSignature& sig) const {
    return sig.pipeline_state_changes == 0;
}

VkImage SynapseCore::get_bound_image(VkCommandBuffer cmd) {
    std::shared_lock<std::shared_mutex> lock(cmd_state_mutex_);
    auto it = cmd_state_.find(cmd);
    return (it != cmd_state_.end()) ? it->second.bound_image : VK_NULL_HANDLE;
}

WorkloadSignature SynapseCore::capture_current_signature(
    VkCommandBuffer cmd, uint32_t count) {
    WorkloadSignature sig{};
    sig.vertex_count = count;
    std::shared_lock<std::shared_mutex> lock(cmd_state_mutex_);
    auto it = cmd_state_.find(cmd);
    if (it != cmd_state_.end()) {
        sig.shader_hash = it->second.shader_hash;
    }
    return sig;
}

void SynapseCore::submit_isa_to_gpu(VkCommandBuffer /*cmd*/,
                                         const std::vector<uint32_t>& /*isa*/) {
    // TODO(T1-3): Call vkCmdBindShadersEXT (VK_EXT_shader_object) with
    //             the compiled ISA binary once per-SKU shader-object support is confirmed.
}

} // namespace synapse
