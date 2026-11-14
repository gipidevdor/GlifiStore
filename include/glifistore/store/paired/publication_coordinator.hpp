/**
 * @file
 * @brief Generation admission and installation decisions before visible publication.
 */
#pragma once

// Read-generation publication helpers (behavior-neutral extraction).
// Normative: docs/spec/mutation-lifecycle.md

#include "glifistore/core/error.hpp"
#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/store/paired/generation_slot_pool.hpp"
#include "glifistore/store/paired/read_generation.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace glifistore::store::paired {

/// A mutation may enter Store only when its resulting read publication has a
/// bounded ownership slot and the current incremental generation can absorb the
/// whole publication batch. This decision is deliberately independent from the
/// execution path (dedicated Writer, combiner, sync or async).
enum class GenerationAdmissionDecision : std::uint8_t {
    admitted,
    reader_quiescence_required,
    incremental_merge_required,
};

[[nodiscard]] constexpr auto decide_generation_admission(const std::size_t retired_generations,
                                                         const std::size_t maximum_retired_generations,
                                                         const bool incremental_publishable) noexcept
    -> GenerationAdmissionDecision {
    if (retired_generations >= maximum_retired_generations) {
        return GenerationAdmissionDecision::reader_quiescence_required;
    }
    if (!incremental_publishable) {
        return GenerationAdmissionDecision::incremental_merge_required;
    }
    return GenerationAdmissionDecision::admitted;
}

[[nodiscard]] constexpr auto generation_admission_message(const GenerationAdmissionDecision decision) noexcept
    -> const char* {
    switch (decision) {
    case GenerationAdmissionDecision::reader_quiescence_required:
        return "mutation rejected until paired Reader reaches quiescence";
    case GenerationAdmissionDecision::incremental_merge_required:
        return "mutation rejected until incremental read merge advances";
    case GenerationAdmissionDecision::admitted:
        break;
    }
    return "";
}

/// Release-publish the raw generation pointer Readers acquire.
inline void publish_read_generation(std::atomic<const PairReadGeneration*>& published,
                                    const PairReadGeneration* generation) noexcept {
    GS_PHASE_PUT(generation_publish);
    GS_FAULT_SITE(publish);
    published.store(generation, std::memory_order_release);
}

/// Release-publish the ADR 0036 slot token and mirror the generation pointer for
/// GET/ReadLease compatibility while Wave 1 dual-publishes both authorities.
inline void publish_read_generation_token(std::atomic<std::uint64_t>& published_token,
                                          std::atomic<const PairReadGeneration*>& published,
                                          const GenerationPublicationToken token,
                                          const PairReadGeneration* generation) noexcept {
    GS_PHASE_PUT(generation_publish);
    GS_FAULT_SITE(publish);
    published.store(generation, std::memory_order_relaxed);
    published_token.store(token.raw, std::memory_order_release);
}

/// Retire previous writer generation and install next after a successful incremental
/// or snapshot publish. Caller owns reclaim / merge flags.
struct GenerationInstall final {
    std::shared_ptr<const PairReadGeneration> previous{};
    std::shared_ptr<const PairReadGeneration> next{};
};

inline void install_writer_generation(std::shared_ptr<const PairReadGeneration>& writer_generation,
                                      std::vector<std::shared_ptr<const PairReadGeneration>>& retired,
                                      std::atomic_size_t& retired_count,
                                      std::atomic<std::uint64_t>& writer_epoch,
                                      const std::size_t maximum_retired_generations,
                                      std::shared_ptr<const PairReadGeneration> next) noexcept {
    // Every caller must have reserved generation ownership before Store entry.
    // Crossing this boundary means publication correctness can no longer be
    // classified safely; terminate instead of growing an allegedly bounded
    // graph or success-ACKing authority that Readers cannot adopt.
    if (!next || decide_generation_admission(retired.size(), maximum_retired_generations, true) !=
                     GenerationAdmissionDecision::admitted) [[unlikely]] {
        std::terminate();
    }
    retired.push_back(writer_generation);
    retired_count.store(retired.size(), std::memory_order_relaxed);
    writer_generation = std::move(next);
    writer_epoch.store(writer_generation->epoch(), std::memory_order_relaxed);
}

} // namespace glifistore::store::paired
