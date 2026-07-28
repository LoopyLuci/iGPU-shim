// ============================================================================\
// synapse/synapse_core.cpp
// Project Synapse – Core routing logic, full Vulkan command interception
// ============================================================================\
#include "synapse_core.h"

#include "hai_frontend_sim.h"
#include <cstring>

namespace synapse {

// ===========================================================================
// Construction / Destruction
// ===========================================================================
SynapseCore::SynapseCore(PFN_vkCmdDrawIndexed orig_draw,
                                        PFN_vkCmdDraw orig_draw_non_indexed,
                                        PFN_vkCmdDispatch orig_dispatch,
                                        PFN_vkCmdPushConstants orig_push_constants,
                                        PFN_vkCmdBindDescriptorSets orig_bind_desc_sets,
                                        PFN_vkCmdBindShadersEXT orig_bind_shaders)
    : orig_draw_indexed_(orig_draw),
      orig_draw_(orig_draw_non_indexed),
      orig_dispatch_(orig_dispatch),
      orig_push_constants_(orig_push_constants),
      orig_bind_descriptor_sets_(orig_bind_desc_sets),
      orig_bind_shaders_(orig_bind_shaders),
      analyzer_(telemetry_),
      scheduler_(analyzer_),
      jit_pipeline_(analyzer_),
      its_engine_(analyzer_)
{
    analyzer_thread_ = std::thread(&Analyzer::process_telemetry_loop, &analyzer_);
}

SynapseCore::~SynapseCore() {
    analyzer_.shutdown();
    if (analyzer_thread_.joinable()) analyzer_thread_.join();
}

// ===========================================================================
// Session report
// ===========================================================================
synapse::telemetry::SynapseSessionReport SynapseCore::build_session_report() const {
    synapse::telemetry::SynapseSessionReport r{};

    // JIT stutter stats
    r.jit.cold_cache_fallbacks = jit_stats_.cold_cache_fallbacks;
    r.jit.worst_fallback_ms    = jit_stats_.worst_fallback_ms;
    r.jit.total_fallback_ms    = jit_stats_.total_fallback_ms;

    // ITS cache — live atomic counters from the hardened engine
    r.its_cache.hits   = its_engine_.get_hits();
    r.its_cache.misses = its_engine_.get_misses();

    // Backend routing summary
    r.backend_routing.jit_dispatches    = backend_jit_count_;
    r.backend_routing.hai_dispatches    = backend_hai_count_;
    r.backend_routing.oracle_dispatches = backend_oracle_count_;
    r.backend_routing.total_draw_calls  =
        backend_jit_count_ + backend_hai_count_ + backend_oracle_count_;

    // ML bandit training stats
    r.ml_model.total_updates     = ml_api_.updates();
    r.ml_model.cumulative_reward = static_cast<double>(ml_api_.cumulative_reward());

    // Power (based on logged transactions so far)
    r.power = power_estimator_.generate();

    return r;
}

// ===========================================================================
// Notification hooks (layer_entry.cpp calls these)
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
// Vulkan command interception — all entry points
// ===========================================================================
void SynapseCore::handle_draw_indexed(
    VkCommandBuffer cmd,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t  vertexOffset,
    uint32_t firstInstance)
{
    WorkloadSignature sig = capture_current_signature(cmd, indexCount);
    telemetry_.push(sig);
    analyzer_.update_its_stats(its_engine_.get_hits(), its_engine_.get_misses());
    its_engine_.prepare_for_use(get_bound_image(cmd), current_frame_);

    const auto t0 = std::chrono::high_resolution_clock::now();
    synapse::telemetry::SynapseSessionReport live_report{};
    live_report.jit.cold_cache_fallbacks = jit_stats_.cold_cache_fallbacks;
    live_report.jit.worst_fallback_ms    = jit_stats_.worst_fallback_ms;
    live_report.jit.total_fallback_ms    = jit_stats_.total_fallback_ms;
    live_report.its_cache.hits   = its_engine_.get_hits();
    live_report.its_cache.misses = its_engine_.get_misses();
    live_report.its_cache.current_usage_bytes = 0; // TODO: wire VRAM allocator
    ExecutionBackend backend = ml_api_.decide(sig, &live_report);

    // Route to backend and count
    increment_backend_counter(backend);
    switch (backend) {
        case ExecutionBackend::JIT:
            execute_jit_path(cmd, sig);
            break;
        case ExecutionBackend::HAI:
            execute_hai_path(cmd, sig);
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
    const uint32_t jit_over_budget = jit_stats_.is_over_budget() ? 1u : 0u;
    const float reward = static_cast<float>(
        reward_calc_.compute(static_cast<float>(elapsed_ms), pr, 0u, jit_over_budget));
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
    telemetry_.push(sig);
    analyzer_.update_its_stats(its_engine_.get_hits(), its_engine_.get_misses());
    its_engine_.prepare_for_use(get_bound_image(cmd), current_frame_);

    ExecutionBackend backend = ml_api_.decide(sig, nullptr);
    increment_backend_counter(backend);

    switch (backend) {
        case ExecutionBackend::JIT:
            execute_jit_path(cmd, sig);
            break;
        case ExecutionBackend::HAI:
            execute_hai_path(cmd, sig);
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
    WorkloadSignature sig;
    sig.is_compute_dispatch     = true;
    sig.draw_call_count         = 1;
    sig.shader_instruction_estimate = 500;
    telemetry_.push(sig);

    ExecutionBackend backend = ml_api_.decide(sig, nullptr);
    increment_backend_counter(backend);

    switch (backend) {
        case ExecutionBackend::JIT:
            execute_jit_path(cmd, sig);
            break;
        case ExecutionBackend::HAI:
            execute_hai_path(cmd, sig);
            break;
        case ExecutionBackend::Oracle:
        default:
            if (orig_dispatch_) {
                orig_dispatch_(cmd, groupCountX, groupCountY, groupCountZ);
            }
            break;
    }
}

void SynapseCore::handle_push_constants(
    VkCommandBuffer cmd,
    VkPipelineLayout layout,
    uint32_t        offset,
    uint32_t        size,
    const void*     pValues)
{
    // Forward to the original driver — the optimizer runs
    // asynchronously on the captured push-constant state.
    if (orig_push_constants_) {
        orig_push_constants_(cmd, layout, offset, size, pValues);
    }
    // TODO: capture the push-constant shadow state for delta encoding
    // in subsequent HAI batches.
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
        std::max(jit_stats_.worst_fallback_ms, elapsed_ms);
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
    auto specialized = jit_pipeline_.get_optimized_shader(sig.shader_hash, ctx_hash);
    if (specialized) {
        submit_isa_to_gpu(cmd, specialized->isa_binary);
    } else {
        execute_oracle_draw(cmd, sig, sig.vertex_count, 1, 0, 0, 0);
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
        // TODO(T1-4): also populate draw_call_count, texture_bindings,
        //             and shader_instruction_estimate from the bound PSO metadata.
    }
    return sig;
}

void SynapseCore::submit_isa_to_gpu(VkCommandBuffer /*cmd*/,
                                         const std::vector<uint32_t>& /*isa*/) {
    // TODO(T1-3): Call vkCmdBindShadersEXT (VK_EXT_shader_object) with
    //             the compiled ISA binary once per-SKU shader-object support is confirmed.
    // Until then, this is a no-op and JIT fallbacks to Oracle on cache miss.
}

} // namespace synapse