#pragma once

// Internal types and helpers for ShardPairRuntime translation units.
// Not installed; behavior-neutral extraction (Phase C decomposition).

#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/config.hpp"
#include "glifistore/store/paired/bounded_spsc_queue.hpp"
#include "glifistore/store/paired/fail_closed_state.hpp"
#include "glifistore/store/paired/lane_publication.hpp"
#include "glifistore/store/paired/lane_state.hpp"
#include "glifistore/store/paired/mutation_slot_pool.hpp"
#include "glifistore/store/paired/publication_coordinator.hpp"
#include "glifistore/store/paired/read_generation.hpp"
#include "glifistore/store/paired/shard_pair_runtime.hpp"
#include "store/store_internal.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace glifistore::store::paired {
namespace runtime_detail {

[[nodiscard]] inline auto elapsed_ns(const std::chrono::steady_clock::time_point start,
                                     const std::chrono::steady_clock::time_point end) noexcept
    -> std::uint64_t {
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return count <= 0 ? 0U : static_cast<std::uint64_t>(count);
}

inline void cpu_relax() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

template <typename Refresh, typename Decide>
[[nodiscard]] inline auto wait_generation_admission(GenerationAdmissionDecision decision, Refresh&& refresh,
                                                    Decide&& decide) -> GenerationAdmissionDecision {
    for (unsigned spin = 0; (decision == GenerationAdmissionDecision::incremental_merge_required ||
                             decision == GenerationAdmissionDecision::reader_quiescence_required) &&
                            spin < 4096U;
         ++spin) {
        if (decision == GenerationAdmissionDecision::reader_quiescence_required) {
            cpu_relax();
            if ((spin & 15U) == 15U) {
                std::this_thread::yield();
            }
        }
        refresh();
        decision = decide();
    }
    return decision;
}

[[nodiscard]] inline auto non_owning_generation_view(const PairReadGeneration* generation) noexcept
    -> std::shared_ptr<const PairReadGeneration> {
    // The pool owns the generation. An empty-owner alias needs no control block.
    return {std::shared_ptr<const PairReadGeneration>{}, generation};
}

template <typename T> inline void atomic_max(std::atomic<T>& destination, const T value) noexcept {
    static_assert(std::is_unsigned_v<T>);
    auto observed = destination.load(std::memory_order_relaxed);
    while (observed < value && !destination.compare_exchange_weak(observed, value, std::memory_order_relaxed,
                                                                  std::memory_order_relaxed)) {
    }
}

inline void atomic_saturating_add(std::atomic<std::uint64_t>& destination,
                                  const std::uint64_t value) noexcept {
    auto observed = destination.load(std::memory_order_relaxed);
    for (;;) {
        const auto next = value > std::numeric_limits<std::uint64_t>::max() - observed
                              ? std::numeric_limits<std::uint64_t>::max()
                              : observed + value;
        if (destination.compare_exchange_weak(observed, next, std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
            return;
        }
    }
}

struct AsyncMutationTask final {
    MutationContext context{};
    std::uint64_t request_id{};
    std::size_t shard{};
    MutationKind kind{};
    std::uint32_t payload_slot{};
    std::uint64_t key_hash{};
    std::uint64_t expire_at_ns{};
    std::size_t admission_bytes{};
    std::chrono::steady_clock::time_point admitted_at{};
    MutationSink sink{};
};

static_assert(std::is_nothrow_move_constructible_v<AsyncMutationTask>);

[[nodiscard]] inline auto deliver_outcome(const MutationSink& sink, MutationOutcome outcome) noexcept
    -> bool {
    if (sink.deliver == nullptr) {
        return false;
    }
    return sink.deliver(sink.completions, std::move(outcome));
}

inline void notify_sink(const MutationSink& sink) noexcept {
    if (sink.notify != nullptr && sink.wakeup != nullptr) {
        sink.notify(sink.wakeup);
    }
}

[[nodiscard]] inline auto has_notification_target(const MutationSink& sink) noexcept -> bool {
    return sink.notify != nullptr && sink.wakeup != nullptr;
}

[[nodiscard]] inline auto same_notification_target(const MutationSink& left,
                                                   const MutationSink& right) noexcept -> bool {
    return left.notify == right.notify && left.wakeup == right.wakeup;
}

inline void note_merge_advance(MergeState& destination, const std::size_t advanced) noexcept {
    destination.read_merge_slots_processed.fetch_add(advanced, std::memory_order_relaxed);
    atomic_max(destination.maximum_read_merge_quantum_slots, static_cast<std::uint64_t>(advanced));
    store_merge_progress(destination);
}

[[nodiscard]] constexpr auto merge_minimum_advance_slots(const std::size_t configured_quantum,
                                                         const std::size_t publication_records) noexcept
    -> std::size_t {
    if (publication_records <= 1U) {
        return configured_quantum;
    }
    return configured_quantum > std::numeric_limits<std::size_t>::max() / 2U
               ? std::numeric_limits<std::size_t>::max()
               : configured_quantum * 2U;
}

[[nodiscard]] inline auto load_generation_memory_stats(const GenerationState& source) noexcept
    -> ReadGenerationMemoryStats {
    ReadGenerationMemoryStats result;
    for (;;) {
        const auto before = source.memory_stats_sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        result.base_entries = source.memory_base_entries.load(std::memory_order_relaxed);
        result.base_capacity = source.memory_base_capacity.load(std::memory_order_relaxed);
        result.base_record_storage_bytes =
            source.memory_base_record_storage_bytes.load(std::memory_order_relaxed);
        result.base_record_mapped_storage_bytes =
            source.memory_base_record_mapped_storage_bytes.load(std::memory_order_relaxed);
        result.base_lookup_storage_bytes =
            source.memory_base_lookup_storage_bytes.load(std::memory_order_relaxed);
        result.base_key_bytes = source.memory_base_key_bytes.load(std::memory_order_relaxed);
        result.base_key_storage_bytes = source.memory_base_key_storage_bytes.load(std::memory_order_relaxed);
        result.base_pin_storage_bytes = source.memory_base_pin_storage_bytes.load(std::memory_order_relaxed);
        result.base_allocated_lower_bound_bytes =
            source.memory_base_allocated_lower_bound_bytes.load(std::memory_order_relaxed);
        result.delta_entries = source.delta_entries.load(std::memory_order_relaxed);
        result.delta_capacity = source.memory_delta_capacity.load(std::memory_order_relaxed);
        result.delta_record_versions = source.delta_record_versions.load(std::memory_order_relaxed);
        result.delta_arena_record_bytes = source.delta_arena_record_bytes.load(std::memory_order_relaxed);
        result.delta_arena_key_bytes = source.delta_arena_key_bytes.load(std::memory_order_relaxed);
        result.delta_arena_key_storage_bytes =
            source.delta_arena_key_storage_bytes.load(std::memory_order_relaxed);
        result.delta_lookup_storage_bytes =
            source.memory_delta_lookup_storage_bytes.load(std::memory_order_relaxed);
        result.delta_allocated_lower_bound_bytes =
            source.memory_delta_allocated_lower_bound_bytes.load(std::memory_order_relaxed);
        result.generation_shell_bytes = source.memory_generation_shell_bytes.load(std::memory_order_relaxed);
        result.current_allocated_lower_bound_bytes =
            source.memory_current_allocated_lower_bound_bytes.load(std::memory_order_relaxed);
        const auto after = source.memory_stats_sequence.load(std::memory_order_acquire);
        if (before == after) {
            return result;
        }
    }
}

} // namespace runtime_detail

struct ShardPairRuntime::SyncMutation final {
    MutationKind kind{};
    ValueType type{ValueType::bytes};
    const HashedKey* key{};
    std::span<const std::byte> value{};
    std::uint64_t expire_at_ns{};
    Status status{};
    std::atomic_bool done{};
    SyncMutation* next{};
};

struct ShardPairRuntime::Lane final {
    Lane(const std::size_t capacity, std::shared_ptr<const PairReadGeneration> initial,
         const std::uint64_t catalog_revision, const std::size_t payload_bytes,
         const std::size_t maximum_payload_bytes)
        : queue(std::max(capacity == 0 ? std::size_t{2} : capacity, std::size_t{2})),
          payloads(capacity == 0 ? 1U : queue.capacity(),
                   capacity == 0 ? 1U : std::max(payload_bytes, std::size_t{1}),
                   capacity == 0 ? 1U
                                 : (maximum_payload_bytes == 0 ? std::max(payload_bytes, std::size_t{1})
                                                               : maximum_payload_bytes)) {
        async.async_enabled = capacity != 0U;
        if (!initial) {
            throw std::runtime_error{"paired Writer has no initial read generation"};
        }
        generation.writer_generation = std::move(initial);
        generation.retired_generations.reserve(ShardPairRuntime::kMaximumRetiredReadGenerations);
        generation.writer_epoch.store(generation.writer_generation->epoch(), std::memory_order_relaxed);
        store_generation_memory_stats(generation, generation.writer_generation->memory_stats());
        generation.published_catalog_revision.store(catalog_revision, std::memory_order_relaxed);
        publish_read_generation(generation.published_generation, generation.writer_generation.get());
    }

    BoundedSpscQueue<runtime_detail::AsyncMutationTask> queue;
    MutationSlotPool payloads;
    std::thread thread;
    SyncMutation* sync_head{};
    AsyncLaneState async{};
    SyncLaneState sync{};
    GenerationState generation{};
    MergeState merge{};
    ReclamationState reclaim{};
    LaneMetrics metrics{};
};

struct WriterAsyncBatchHooks final {
    // Non-owning pointers to Writer-loop-stable functors (same discipline as sync).
    const std::function<void()>* publish_fail_closed{};
    const std::function<void()>* reclaim_quiescent{};
    const std::function<void(std::size_t)>* process_merge{};
    const std::function<bool()>* drain_durable_snapshot{};
    const std::function<void()>* update_delta_stats{};
};

struct WriterAsyncBatchEnv final {
    ShardPairRuntime::Lane& lane;
    std::size_t shard{};
    std::optional<DurableGroupConfig> batch_config;
    std::size_t maximum_batch_records{};
    std::size_t maximum_batch_bytes{};
    std::vector<runtime_detail::AsyncMutationTask>& batch;
    std::optional<runtime_detail::AsyncMutationTask>& carried_task;
    bool& merge_retry_blocked;
    FailClosedState& fail_closed;
    LanePublicationContext& publication_ctx;
    std::vector<std::uint64_t>& queue_waits;
    std::vector<std::chrono::steady_clock::time_point>& service_started;
    std::vector<bool>& expired;
    std::vector<MutationOutcome>& completions;
    std::vector<detail::StoreAccess::DurableMutationView>& durable_views;
    std::vector<std::size_t>& durable_indices;
    std::vector<ReadMutation>& read_mutations;
    std::vector<std::size_t>& read_mutation_indices;
    WriterAsyncBatchHooks hooks;
};

struct WriterSyncDrainHooks final {
    // Non-owning pointers to Writer-loop-stable functors. Assigning std::function
    // into these each drain turn allocates and is unsafe under process_fail_at.
    const std::function<void()>* publish_fail_closed{};
    const std::function<void()>* sticky_pair_before_durable_mark{};
    const std::function<void()>* reclaim_quiescent{};
    const std::function<void()>* reclaim_proportional{};
    const std::function<bool()>* drain_durable_snapshot{};
    const std::function<void(std::size_t)>* process_merge{};
    const std::function<void()>* update_delta_stats{};
    const std::function<void(std::size_t)>* prepare_publish_retry{};
};

struct WriterSyncDrainEnv final {
    ShardPairRuntime::Lane& lane;
    std::size_t shard{};
    std::optional<DurableGroupConfig> batch_config;
    ShardPairRuntime::SyncMutation*& carried_sync;
    bool& merge_retry_blocked;
    LanePublicationContext& publication_ctx;
    WriterSyncDrainHooks hooks;
};

} // namespace glifistore::store::paired
