// ============================================================================
// synapse/hash_utils.h
// Project Synapse – Canonical Hash Utilities (Single Source of Truth)
//
// Previously duplicated in synapse_core.h and synapse_jit_backend.h.
// All modules MUST include this header and MUST NOT re-implement hash_combine.
// ============================================================================
#pragma once

#include <cstdint>
#include "synapse_umd.h"   // WorkloadSignature, ExecutionBackend

namespace synapse::util {

// ----------------------------------------------------------------------------
// hash_combine
// Standard Boost-style hash combining. Satisfies the avalanche property.
// Formula:  h ^= (v + 0x9e3779b9 + (h << 6) + (h >> 2))
// ----------------------------------------------------------------------------
inline uint64_t hash_combine(uint64_t seed, uint64_t value) noexcept {
    seed ^= value + 0x9e3779b9ULL + (seed << 6) + (seed >> 2);
    return seed;
}

// ----------------------------------------------------------------------------
// workload_context_hash
// Combines a shader identifier with live workload conditions so that the same
// shader compiled for different telemetry states produces a distinct cache key.
//
// @param sig  Current WorkloadSignature captured from the render thread.
// @return     A 64-bit context hash for use in JITSpecializationCache.
// ----------------------------------------------------------------------------
inline uint64_t workload_context_hash(const WorkloadSignature& sig) noexcept {
    uint64_t h = sig.shader_hash;
    h = hash_combine(h, sig.draw_call_count);
    h = hash_combine(h, sig.shader_instruction_estimate);
    h = hash_combine(h, sig.vertex_count);
    h = hash_combine(h, static_cast<uint64_t>(sig.is_compute_dispatch ? 1 : 0));
    return h;
}

// ----------------------------------------------------------------------------
// backend_context_hash
// Used inside JITPipeline to distinguish compilations that differ only by the
// recommended ExecutionBackend (e.g., JIT vs. HAI specialisation path).
//
// @param shader_hash  Identity hash of the shader source.
// @param backend      The execution backend driving this compilation.
// @return             A 64-bit key for JITSpecializationCache lookup.
// ----------------------------------------------------------------------------
inline uint64_t backend_context_hash(uint64_t shader_hash,
                                     ExecutionBackend backend) noexcept {
    return hash_combine(shader_hash, static_cast<uint64_t>(backend) << 32);
}

} // namespace synapse::util
