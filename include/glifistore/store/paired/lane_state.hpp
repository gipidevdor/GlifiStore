#pragma once

// By-value Lane sub-aggregates for ShardPairRuntime (behavior-neutral layout).
// Normative: docs/spec/mutation-lifecycle.md Phase 4
//
// Cache-line policy (Wave 2):
//   * Cross-thread atomics each sit alone on an alignas(128) line (Reader↔Writer).
//   * Writer-local handles and telemetry pack together after a 128-byte barrier so
//     they do not false-share with Reader-hot publish / lease fields.
//   * LaneMetrics is Writer-updated / stats-polled; it starts on its own line so
//     dense counter RMWs do not sit on ReclamationState::active_read_leases.

#include "glifistore/core/latency_histogram.hpp"
#include "glifistore/store/paired/generation_slot_pool.hpp"
#include "glifistore/store/paired/read_generation.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace glifistore::store::paired {

struct AtomicLatencyHistogram final {
    std::array<std::atomic<std::uint64_t>, LatencyHistogram::kBoundsNs.size()> counts{};
    std::atomic<std::uint64_t> observations{};
    std::atomic<std::uint64_t> sum_ns{};

    void observe(const std::uint64_t sample_ns) noexcept {
        for (std::size_t index = 0; index < LatencyHistogram::kBoundsNs.size(); ++index) {
            if (sample_ns <= LatencyHistogram::kBoundsNs[index]) {
                counts[index].fetch_add(1U, std::memory_order_relaxed);
                break;
            }
        }
        observations.fetch_add(1U, std::memory_order_relaxed);
        auto observed = sum_ns.load(std::memory_order_relaxed);
        for (;;) {
            const auto next = sample_ns > std::numeric_limits<std::uint64_t>::max() - observed
                                  ? std::numeric_limits<std::uint64_t>::max()
                                  : observed + sample_ns;
            if (sum_ns.compare_exchange_weak(observed, next, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
                return;
            }
        }
    }

    [[nodiscard]] auto snapshot() const noexcept -> LatencyHistogram {
        LatencyHistogram result;
        for (std::size_t index = 0; index < counts.size(); ++index) {
            result.counts[index] = counts[index].load(std::memory_order_relaxed);
        }
        result.observations = observations.load(std::memory_order_relaxed);
        result.sum_ns = sum_ns.load(std::memory_order_relaxed);
        return result;
    }
};

struct AsyncLaneState final {
    // Reader/submitter ↔ Writer rendezvous (each alone on a line).
    alignas(128) std::mutex queue_consumer_mutex{};
    alignas(128) std::atomic<std::uint64_t> signal{};
    alignas(128) std::atomic_bool stopping{};
    // ADR 0037: IDLE=0 / EXECUTING=1 sole mutator ownership for the shard.
    alignas(128) std::atomic<std::uint32_t> execution_token{};
    // Admission-byte counter: producers and Writer; isolate from the token line.
    alignas(128) std::atomic<std::size_t> queued_bytes{};
    bool async_enabled{};
};

struct SyncLaneState final {
    alignas(128) std::mutex sync_mutex{};
    alignas(128) std::atomic<std::uint64_t> sync_admitted{};
};

struct GenerationState final {
    // --- Writer-local ownership (starts on a 128 B boundary via struct alignof) ---
    std::unique_ptr<GenerationSlotPool> slot_pool;
    std::shared_ptr<const PairReadGeneration> writer_generation;
    std::vector<std::shared_ptr<const PairReadGeneration>> retired_generations;

    // --- Reader-hot / publish authority (each alone on a line) ---
    alignas(128) std::atomic<const PairReadGeneration*> published_generation{};
    alignas(128) std::atomic<std::uint64_t> published_token{};
    alignas(128) std::atomic<std::uint64_t> writer_epoch{};
    alignas(128) std::atomic<std::uint64_t> reader_safe_epoch{};
    alignas(128) std::atomic<std::uint64_t> published_catalog_revision{};
    alignas(128) std::atomic_bool refresh_requested{};
    alignas(128) std::atomic_bool reclaim_requested{};

    // --- Writer telemetry / census (isolated from Reader-hot reclaim_requested) ---
    alignas(128) std::atomic<std::uint64_t> generations_retired{};
    std::atomic<std::uint64_t> shutdown_generations_reclaimed{};
    std::atomic<std::uint64_t> generation_admission_backpressure_total{};
    std::atomic_bool reader_shutdown_finalized{};
    std::atomic<std::size_t> retired_generation_count{};
    std::atomic<std::size_t> delta_entries{};
    std::atomic<std::size_t> delta_record_versions{};
    std::atomic<std::size_t> delta_arena_record_bytes{};
    std::atomic<std::size_t> delta_arena_key_bytes{};
    std::atomic<std::size_t> delta_arena_key_storage_bytes{};
    // Even values delimit one coherent Writer-published memory census; odd
    // values mean the relaxed payload fields are being replaced.
    std::atomic<std::uint64_t> memory_stats_sequence{};
    std::atomic<std::size_t> memory_base_entries{};
    std::atomic<std::size_t> memory_base_capacity{};
    std::atomic<std::size_t> memory_base_record_storage_bytes{};
    std::atomic<std::size_t> memory_base_record_mapped_storage_bytes{};
    std::atomic<std::size_t> memory_base_lookup_storage_bytes{};
    std::atomic<std::size_t> memory_base_key_bytes{};
    std::atomic<std::size_t> memory_base_key_storage_bytes{};
    std::atomic<std::size_t> memory_base_pin_storage_bytes{};
    std::atomic<std::size_t> memory_base_allocated_lower_bound_bytes{};
    std::atomic<std::size_t> memory_delta_capacity{};
    std::atomic<std::size_t> memory_delta_lookup_storage_bytes{};
    std::atomic<std::size_t> memory_delta_allocated_lower_bound_bytes{};
    std::atomic<std::size_t> memory_generation_shell_bytes{};
    std::atomic<std::size_t> memory_current_allocated_lower_bound_bytes{};

    [[nodiscard]] auto uses_slot_pool() const noexcept -> bool {
        return slot_pool != nullptr;
    }

    [[nodiscard]] auto retired_debt() const noexcept -> std::size_t {
        return uses_slot_pool() ? slot_pool->retired_count() : retired_generations.size();
    }
};

struct MergeState final {
    std::unique_ptr<PairReadMerge> read_merge;
    std::atomic_bool read_merge_active{};
    std::atomic<std::size_t> read_merge_post_entries{};
    std::atomic<std::uint64_t> read_merge_starts{};
    std::atomic<std::uint64_t> read_merge_completions{};
    std::atomic<std::uint64_t> read_merge_failures{};
    std::atomic<std::uint64_t> read_merge_backpressure{};
    std::atomic<std::uint64_t> read_merge_slots_processed{};
    std::atomic<std::size_t> read_merge_remaining_slots{};
    std::atomic<std::size_t> read_merge_post_capacity_remaining{};
    std::atomic<std::uint64_t> maximum_read_merge_quantum_slots{};
    // Writer-local: resource_exhausted merge start defers retries until a publish clears it.
    bool merge_retry_blocked{};
};

struct ReclamationState final {
    alignas(128) std::atomic_size_t active_read_leases{};
};

struct alignas(128) LaneMetrics final {
    std::atomic<std::size_t> maximum_queue_depth{};
    std::atomic<std::size_t> maximum_queued_bytes{};
    std::atomic<std::size_t> payload_slots_in_use{};
    std::atomic<std::size_t> maximum_payload_slots_in_use{};
    std::atomic<std::size_t> payload_arena_bytes_in_use{};
    std::atomic<std::size_t> maximum_payload_arena_bytes_in_use{};
    std::atomic<std::size_t> payload_admission_bytes_in_use{};
    std::atomic<std::size_t> maximum_payload_admission_bytes_in_use{};
    std::atomic<std::uint64_t> payload_slot_full_total{};
    std::atomic<std::uint64_t> payload_arena_full_total{};
    std::atomic<std::uint64_t> payload_too_large_total{};
    std::atomic<std::uint64_t> admitted{};
    std::atomic<std::uint64_t> rejected{};
    std::atomic<std::uint64_t> expired_before_store{};
    std::atomic<std::uint64_t> completed{};
    std::atomic<std::uint64_t> conflict_retries{};
    std::atomic<std::uint64_t> conflict_retry_commits{};
    std::atomic<std::uint64_t> writer_batches{};
    std::atomic<std::uint64_t> writer_batch_records{};
    std::atomic<std::size_t> maximum_writer_batch_records{};
    std::atomic<std::uint64_t> total_writer_batch_wait_ns{};
    std::atomic<std::uint64_t> maximum_writer_batch_wait_ns{};
    std::atomic<std::uint64_t> writer_batch_durability_deadline_closes{};
    std::atomic<std::uint64_t> writer_batch_queue_deadline_closes{};
    std::atomic<std::uint64_t> sync_drain_turns{};
    std::atomic<std::uint64_t> sync_turn_splits{};
    std::atomic<std::uint64_t> sync_async_fairness_turns{};
    std::atomic<std::uint64_t> publications{};
    std::atomic<std::uint64_t> publication_records{};
    std::atomic<std::uint64_t> completion_notifications{};
    std::atomic<std::uint64_t> total_queue_wait_ns{};
    std::atomic<std::uint64_t> maximum_queue_wait_ns{};
    std::atomic<std::uint64_t> total_service_ns{};
    std::atomic<std::uint64_t> maximum_service_ns{};
    std::atomic<std::uint64_t> read_refresh_attempts{};
    std::atomic<std::uint64_t> read_refresh_successes{};
    std::atomic<std::uint64_t> read_refresh_failures{};
    std::atomic<std::uint64_t> read_refresh_deferrals{};
    AtomicLatencyHistogram queue_wait_histogram{};
    AtomicLatencyHistogram service_histogram{};
};

static_assert(alignof(std::atomic_size_t) <= 128);
static_assert(sizeof(std::atomic_size_t) <= 128);
static_assert(alignof(AsyncLaneState) >= 128);
static_assert(alignof(GenerationState) >= 128);
static_assert(alignof(SyncLaneState) >= 128);
static_assert(alignof(ReclamationState) >= 128);
static_assert(alignof(LaneMetrics) >= 128);
static_assert(std::is_nothrow_default_constructible_v<AsyncLaneState>);
static_assert(std::is_nothrow_default_constructible_v<LaneMetrics>);

} // namespace glifistore::store::paired
