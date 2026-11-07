#include "glifistore/store/paired/shard_pair_runtime.hpp"

#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/hot_path_phases.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/store/paired/bounded_spsc_queue.hpp"
#include "glifistore/store/paired/completion_policy.hpp"
#include "glifistore/store/paired/fail_closed_state.hpp"
#include "glifistore/store/paired/lane_publication.hpp"
#include "glifistore/store/paired/lane_state.hpp"
#include "glifistore/store/paired/mutation_batch.hpp"
#include "glifistore/store/paired/mutation_execution.hpp"
#include "glifistore/store/paired/mutation_recovery.hpp"
#include "glifistore/store/paired/mutation_slot_pool.hpp"
#include "glifistore/store/paired/mutation_state.hpp"
#include "glifistore/store/paired/publication_coordinator.hpp"
#include "glifistore/store/paired/shard_combining_executor.hpp"
#include "glifistore/store/paired/volatile_sync_chunk.hpp"
#include "store/paired/shard_pair_runtime_impl.hpp"
#include "store/store_internal.hpp"

#include <type_traits>

namespace glifistore::store::paired {

ShardPairRuntime::ShardPairRuntime(Store& store, PairedConcurrencyConfig config,
                                   std::vector<std::shared_ptr<const PairReadGeneration>> initial_generations,
                                   std::vector<std::uint64_t> initial_catalog_revisions)
    : store_(store), config_(config),
      maximum_queue_wait_(std::chrono::milliseconds{config_.async_queue_wait_ms}) {
    const auto shard_count = initial_generations.size();
    if (initial_catalog_revisions.size() != shard_count) {
        throw std::invalid_argument{"paired Writer initial generation count mismatch"};
    }
    lanes_.reserve(shard_count);
    fail_closed_wakes_.reserve(shard_count);
    for (std::size_t shard = 0; shard < shard_count; ++shard) {
        lanes_.push_back(
            std::make_unique<Lane>(config_.async_lane_capacity, std::move(initial_generations[shard]),
                                   initial_catalog_revisions[shard], config_.async_lane_payload_bytes,
                                   config_.async_maximum_payload_bytes));
        fail_closed_wakes_.push_back(FailClosedLaneWake{.signal = &lanes_.back()->async.signal});
    }
}

ShardPairRuntime::~ShardPairRuntime() {
    static_cast<void>(stop_and_drain());
}

auto ShardPairRuntime::async_lane_enabled() const noexcept -> bool {
    return !lanes_.empty() && lanes_.front()->async.async_enabled;
}

auto ShardPairRuntime::dedicated_writer_required() const noexcept -> bool {
    if (async_lane_enabled()) {
        return true;
    }
    // durable_group / Writer-batch modes keep the dedicated coalesce loop.
    return detail::StoreAccess::is_durable(store_) &&
           detail::StoreAccess::durable_writer_batch_config(store_).has_value();
}

auto ShardPairRuntime::create(Store& store, const PairedConcurrencyConfig& config)
    -> Result<std::unique_ptr<ShardPairRuntime>> try {
    const auto shard_count = store.worker_count();
    if (shard_count == 0 || config.merge_delta_entries == 0 || config.merge_maximum_post_entries == 0 ||
        config.merge_quantum_slots == 0 ||
        config.merge_delta_entries > PairReadGeneration::kMaximumIncrementalDeltaEntries ||
        config.merge_maximum_post_entries >
            PairReadGeneration::kMaximumIncrementalDeltaEntries - config.merge_delta_entries ||
        (config.async_lane_capacity != 0 &&
         (config.async_lane_payload_bytes == 0 || config.async_writer_batch_max_records == 0 ||
          config.async_writer_batch_max_bytes == 0))) {
        return fail(ErrorCode::invalid_argument, "paired runtime configuration is invalid");
    }
    const auto routing = detail::StoreAccess::worker_routing(store);
    std::vector<std::shared_ptr<const PairReadGeneration>> initial_generations;
    std::vector<std::uint64_t> initial_catalog_revisions;
    std::vector<std::unique_ptr<GenerationSlotPool>> slot_pools;
    initial_generations.reserve(shard_count);
    initial_catalog_revisions.reserve(shard_count);
    if (config.generation_slot_pool) {
        slot_pools.reserve(shard_count);
    }
    for (std::size_t shard = 0; shard < shard_count; ++shard) {
        std::span<const DurableRuntimeCatalog::PublishedReadRecord> records{};
        std::vector<DurableRuntimeCatalog::PublishedReadRecord> durable_records;
        std::uint64_t catalog_revision = 0U;
        if (detail::StoreAccess::is_durable(store)) {
            auto snapshot = detail::StoreAccess::snapshot_durable_reads(store, shard);
            if (!snapshot) {
                return unexpected(snapshot.error());
            }
            durable_records = std::move(snapshot->records);
            records = durable_records;
            catalog_revision = snapshot->catalog_revision;
        }
        initial_catalog_revisions.push_back(catalog_revision);
        if (config.generation_slot_pool) {
            auto pool = GenerationSlotPool::create(routing, records);
            if (!pool) {
                return unexpected(std::move(pool.error()));
            }
            initial_generations.push_back(
                runtime_detail::non_owning_generation_view((*pool)->writer_generation()));
            slot_pools.push_back(std::move(*pool));
            continue;
        }
        Result<std::shared_ptr<const PairReadGeneration>> initial = PairReadGeneration::empty(routing);
        if (!records.empty()) {
            initial = PairReadGeneration::from_durable_snapshot(routing, records);
        }
        if (!initial) {
            return unexpected(initial.error());
        }
        initial_generations.push_back(std::move(*initial));
    }
    auto runtime = std::unique_ptr<ShardPairRuntime>(new ShardPairRuntime(
        store, config, std::move(initial_generations), std::move(initial_catalog_revisions)));
    if (config.generation_slot_pool) {
        for (std::size_t shard = 0; shard < shard_count; ++shard) {
            auto& generation = runtime->lanes_[shard]->generation;
            generation.slot_pool = std::move(slot_pools[shard]);
            generation.slot_pool->set_failure_hook(GenerationSlotFailureHook{
                .context = runtime.get(),
                .fail_closed =
                    [](void* context) noexcept {
                        static_cast<ShardPairRuntime*>(context)->trip_generation_slot_fail_closed();
                    },
            });
            mirror_slot_pool_publication(generation);
        }
    }
    return runtime;
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "paired runtime allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "paired runtime construction failed");
}

auto ShardPairRuntime::start() -> Status try {
    if (started_.exchange(true, std::memory_order_acq_rel)) {
        return fail(ErrorCode::invalid_argument, "paired mutation executor has already been started");
    }
    if (stopping_.load(std::memory_order_acquire)) {
        return fail(ErrorCode::unavailable, "paired mutation executor has been stopped");
    }
    // ADR 0037 Phase A/B: embedded volatile and durable_sync combine on callers;
    // no permanent Writer threads unless async or durable_group batching requires them.
    if (!dedicated_writer_required()) {
        return {};
    }
    for (std::size_t worker = 0; worker < lanes_.size(); ++worker) {
        active_writers_.fetch_add(1U, std::memory_order_relaxed);
        try {
            lanes_[worker]->thread = std::thread{[this, worker] { run(worker); }};
        } catch (...) {
            active_writers_.fetch_sub(1U, std::memory_order_relaxed);
            throw;
        }
    }
    return {};
} catch (const std::exception& exception) {

    static_cast<void>(stop_and_drain());
    return fail(ErrorCode::io_error,
                std::string{"failed to start paired mutation executor: "} + exception.what());
} catch (...) {
    static_cast<void>(stop_and_drain());
    return fail(ErrorCode::io_error, "failed to start paired mutation executor");
}

auto ShardPairRuntime::begin_submission() noexcept -> bool {
    const auto previous = admission_state_.fetch_add(1U, std::memory_order_acq_rel);
    if ((previous & kAdmissionClosed) == 0) {
        return true;
    }
    finish_submission();
    return false;
}

void ShardPairRuntime::finish_submission() noexcept {
    const auto previous = admission_state_.fetch_sub(1U, std::memory_order_acq_rel);
    if ((previous & kAdmissionClosed) != 0 && (previous & kAdmissionCountMask) == 1U) {
        admission_state_.notify_all();
    }
}

auto ShardPairRuntime::mutation_admission_bytes(const std::size_t key_bytes,
                                                const std::size_t value_bytes) noexcept
    -> std::optional<std::size_t> {
    if (key_bytes > std::numeric_limits<std::size_t>::max() - value_bytes) {
        return std::nullopt;
    }
    const auto payload_bytes = key_bytes + value_bytes;
    if (payload_bytes > std::numeric_limits<std::size_t>::max() - kMutationAdmissionOverheadBytes) {
        return std::nullopt;
    }
    return kMutationAdmissionOverheadBytes + payload_bytes;
}

auto ShardPairRuntime::try_submit(const AsyncMutationRequest& request) noexcept
    -> std::optional<std::size_t> {
    if (request.shard >= lanes_.size()) {
        return std::nullopt;
    }
    auto& lane = *lanes_[request.shard];
    if (!lanes_[request.shard]->async.async_enabled || request.sink.deliver == nullptr) {
        lane.metrics.rejected.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
    }
    if (!begin_submission()) {
        lane.metrics.rejected.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
    }

    struct SubmissionGuard final {
        ShardPairRuntime& executor;

        ~SubmissionGuard() {
            executor.finish_submission();
        }
    } submission{*this};

    if (!healthy_.load(std::memory_order_acquire) || !started_.load(std::memory_order_acquire) ||
        stopping_.load(std::memory_order_acquire) || lane.async.stopping.load(std::memory_order_acquire) ||
        expire_remaining_.load(std::memory_order_acquire) || !detail::StoreAccess::admissions_open(store_)) {
        lane.metrics.rejected.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
    }
    const auto admission_bytes = mutation_admission_bytes(request.key.size(), request.value.size());
    if (!admission_bytes) {
        lane.metrics.payload_too_large_total.fetch_add(1U, std::memory_order_relaxed);
        lane.metrics.rejected.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
    }
    const auto queued_bytes = lane.async.queued_bytes.load(std::memory_order_relaxed);
    if (*admission_bytes > std::numeric_limits<std::size_t>::max() - queued_bytes) {
        lane.metrics.rejected.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
    }
    auto acquired = lane.payloads.try_acquire(request.key, request.value, *admission_bytes);
    if (!acquired.lease) {
        switch (acquired.failure) {
        case MutationSlotPool::AcquireFailure::slot_exhausted:
            lane.metrics.payload_slot_full_total.fetch_add(1U, std::memory_order_relaxed);
            break;
        case MutationSlotPool::AcquireFailure::byte_exhausted:
            lane.metrics.payload_arena_full_total.fetch_add(1U, std::memory_order_relaxed);
            break;
        case MutationSlotPool::AcquireFailure::payload_too_large:
            lane.metrics.payload_too_large_total.fetch_add(1U, std::memory_order_relaxed);
            break;
        case MutationSlotPool::AcquireFailure::none:
            break;
        }
        lane.metrics.rejected.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
    }
    runtime_detail::AsyncMutationTask task{.context = request.context,
                                           .request_id = request.request_id,
                                           .shard = request.shard,
                                           .kind = request.kind,
                                           .payload_slot = acquired.lease->slot,
                                           .key_hash = request.key_hash,
                                           .expire_at_ns = request.expire_at_ns,
                                           .admission_bytes = acquired.lease->admission_bytes,
                                           .admitted_at = std::chrono::steady_clock::now(),
                                           .sink = request.sink};
    const auto next_bytes =
        lane.async.queued_bytes.fetch_add(*admission_bytes, std::memory_order_relaxed) + *admission_bytes;
    GS_FAULT_SITE(enqueue);
    if (!lane.queue.try_push(task)) {
        lane.async.queued_bytes.fetch_sub(*admission_bytes, std::memory_order_relaxed);
        if (!lane.payloads.rollback(*acquired.lease)) {
            std::terminate();
        }
        lane.metrics.rejected.fetch_add(1U, std::memory_order_relaxed);
        return std::nullopt;
    }
    const auto slots_in_use = lane.payloads.slots_in_use();
    const auto arena_bytes_in_use = lane.payloads.payload_bytes_in_use();
    const auto admission_bytes_in_use = lane.payloads.admission_bytes_in_use();
    lane.metrics.payload_slots_in_use.store(slots_in_use, std::memory_order_relaxed);
    lane.metrics.payload_arena_bytes_in_use.store(arena_bytes_in_use, std::memory_order_relaxed);
    lane.metrics.payload_admission_bytes_in_use.store(admission_bytes_in_use, std::memory_order_relaxed);
    runtime_detail::atomic_max(lane.metrics.maximum_payload_slots_in_use, slots_in_use);
    runtime_detail::atomic_max(lane.metrics.maximum_payload_arena_bytes_in_use, arena_bytes_in_use);
    runtime_detail::atomic_max(lane.metrics.maximum_payload_admission_bytes_in_use, admission_bytes_in_use);
    lane.metrics.admitted.fetch_add(1U, std::memory_order_relaxed);
    runtime_detail::atomic_max(lane.metrics.maximum_queue_depth, lane.queue.size());
    runtime_detail::atomic_max(lane.metrics.maximum_queued_bytes, next_bytes);
    wake(lane);
    return admission_bytes;
}

auto ShardPairRuntime::release_payload(const std::size_t shard, const std::uint32_t payload_slot) noexcept
    -> bool {
    if (shard >= lanes_.size()) {
        return false;
    }
    auto& lane = *lanes_[shard];
    if (!lane.payloads.release(payload_slot)) {
        return false;
    }
    lane.metrics.payload_slots_in_use.store(lane.payloads.slots_in_use(), std::memory_order_relaxed);
    lane.metrics.payload_arena_bytes_in_use.store(lane.payloads.payload_bytes_in_use(),
                                                  std::memory_order_relaxed);
    lane.metrics.payload_admission_bytes_in_use.store(lane.payloads.admission_bytes_in_use(),
                                                      std::memory_order_relaxed);
    return true;
}

void ShardPairRuntime::trip_generation_slot_fail_closed() noexcept {
    FailClosedState fail_closed{store_, healthy_, expire_remaining_};
    fail_closed.arm(fail_closed_wakes_, FailClosedScope::pair_and_store);
}

auto ShardPairRuntime::adopt_read_generation(const std::size_t shard,
                                             const std::uint64_t minimum_leased_epoch) const noexcept
    -> const PairReadGeneration* {
    if (shard >= lanes_.size()) {
        return nullptr;
    }
    auto& lane = *lanes_[shard];
    GS_FAULT_SITE(adopt);
    const PairReadGeneration* generation = load_published_generation(lane.generation);
    if (generation != nullptr) {
        // The current turn protects generation->epoch(); asynchronous I/O can
        // additionally keep an older epoch borrowed. The minimum is the sole
        // reclamation boundary published by Reader to Writer.
        // Skip the acq_rel RMW when the published frontier is unchanged (Wave 2):
        // redundant release-stores on the hot GET/adopt path woke no one and
        // dirtied the Reader-hot cache line every turn.
        const auto safe_epoch = std::min(generation->epoch(), minimum_leased_epoch);
        auto expected = lane.generation.reader_safe_epoch.load(std::memory_order_relaxed);
        while (expected != safe_epoch) {
            if (lane.generation.reader_safe_epoch.compare_exchange_weak(
                    expected, safe_epoch, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                if (!lane.generation.reclaim_requested.exchange(true, std::memory_order_acq_rel)) {
                    lane.async.signal.fetch_add(1U, std::memory_order_release);
                    lane.async.signal.notify_one();
                }
                break;
            }
        }
    }
    return generation;
}

void ShardPairRuntime::request_read_refresh(const std::size_t shard) noexcept {
    if (shard >= lanes_.size()) {
        return;
    }
    auto& lane = *lanes_[shard];
    const auto current = detail::StoreAccess::durable_read_catalog_revision(store_, shard);
    if (current == 0U ||
        current == lane.generation.published_catalog_revision.load(std::memory_order_acquire)) {
        return;
    }
    if (!lane.generation.refresh_requested.exchange(true, std::memory_order_acq_rel)) {
        lane.async.signal.fetch_add(1U, std::memory_order_release);
        lane.async.signal.notify_one();
    }
}

void ShardPairRuntime::note_rejected(const std::size_t shard) noexcept {
    if (shard < lanes_.size()) {
        lanes_[shard]->metrics.rejected.fetch_add(1U, std::memory_order_relaxed);
    }
}

auto ShardPairRuntime::stats() const -> std::vector<ShardPairStats> {
    std::vector<ShardPairStats> result;
    result.reserve(lanes_.size());
    for (std::size_t worker = 0; worker < lanes_.size(); ++worker) {
        const auto& lane = *lanes_[worker];
        const auto generation_memory = runtime_detail::load_generation_memory_stats(lane.generation);
        result.push_back(
            {.worker_index = worker,
             .reader_safe_epoch = lane.generation.reader_safe_epoch.load(std::memory_order_relaxed),
             .writer_epoch = lane.generation.writer_epoch.load(std::memory_order_relaxed),
             .queue_depth = lane.queue.size(),
             .queued_bytes = lane.async.queued_bytes.load(std::memory_order_relaxed),
             .maximum_queue_depth = lane.metrics.maximum_queue_depth.load(std::memory_order_relaxed),
             .maximum_queued_bytes = lane.metrics.maximum_queued_bytes.load(std::memory_order_relaxed),
             .payload_slot_capacity = lane.payloads.slot_capacity(),
             .payload_slots_in_use = lane.metrics.payload_slots_in_use.load(std::memory_order_relaxed),
             .maximum_payload_slots_in_use =
                 lane.metrics.maximum_payload_slots_in_use.load(std::memory_order_relaxed),
             .payload_arena_capacity_bytes = lane.payloads.byte_capacity(),
             .payload_arena_storage_bytes = lane.payloads.storage_bytes(),
             .payload_arena_bytes_in_use =
                 lane.metrics.payload_arena_bytes_in_use.load(std::memory_order_relaxed),
             .maximum_payload_arena_bytes_in_use =
                 lane.metrics.maximum_payload_arena_bytes_in_use.load(std::memory_order_relaxed),
             .payload_admission_bytes_in_use =
                 lane.metrics.payload_admission_bytes_in_use.load(std::memory_order_relaxed),
             .maximum_payload_admission_bytes_in_use =
                 lane.metrics.maximum_payload_admission_bytes_in_use.load(std::memory_order_relaxed),
             .payload_slot_full_total = lane.metrics.payload_slot_full_total.load(std::memory_order_relaxed),
             .payload_arena_full_total =
                 lane.metrics.payload_arena_full_total.load(std::memory_order_relaxed),
             .payload_too_large_total = lane.metrics.payload_too_large_total.load(std::memory_order_relaxed),
             .admitted = lane.metrics.admitted.load(std::memory_order_relaxed),
             .rejected = lane.metrics.rejected.load(std::memory_order_relaxed),
             .expired_before_store = lane.metrics.expired_before_store.load(std::memory_order_relaxed),
             .completed = lane.metrics.completed.load(std::memory_order_relaxed),
             .conflict_retries = lane.metrics.conflict_retries.load(std::memory_order_relaxed),
             .conflict_retry_commits = lane.metrics.conflict_retry_commits.load(std::memory_order_relaxed),
             .writer_batches = lane.metrics.writer_batches.load(std::memory_order_relaxed),
             .writer_batch_records = lane.metrics.writer_batch_records.load(std::memory_order_relaxed),
             .maximum_writer_batch_records =
                 lane.metrics.maximum_writer_batch_records.load(std::memory_order_relaxed),
             .total_writer_batch_wait_ns =
                 lane.metrics.total_writer_batch_wait_ns.load(std::memory_order_relaxed),
             .maximum_writer_batch_wait_ns =
                 lane.metrics.maximum_writer_batch_wait_ns.load(std::memory_order_relaxed),
             .writer_batch_durability_deadline_closes =
                 lane.metrics.writer_batch_durability_deadline_closes.load(std::memory_order_relaxed),
             .writer_batch_queue_deadline_closes =
                 lane.metrics.writer_batch_queue_deadline_closes.load(std::memory_order_relaxed),
             .sync_drain_turns = lane.metrics.sync_drain_turns.load(std::memory_order_relaxed),
             .sync_turn_splits = lane.metrics.sync_turn_splits.load(std::memory_order_relaxed),
             .sync_async_fairness_turns =
                 lane.metrics.sync_async_fairness_turns.load(std::memory_order_relaxed),
             .publications = lane.metrics.publications.load(std::memory_order_relaxed),
             .publication_records = lane.metrics.publication_records.load(std::memory_order_relaxed),
             .completion_notifications =
                 lane.metrics.completion_notifications.load(std::memory_order_relaxed),
             .total_queue_wait_ns = lane.metrics.total_queue_wait_ns.load(std::memory_order_relaxed),
             .maximum_queue_wait_ns = lane.metrics.maximum_queue_wait_ns.load(std::memory_order_relaxed),
             .total_service_ns = lane.metrics.total_service_ns.load(std::memory_order_relaxed),
             .maximum_service_ns = lane.metrics.maximum_service_ns.load(std::memory_order_relaxed),
             .read_catalog_revision =
                 lane.generation.published_catalog_revision.load(std::memory_order_relaxed),
             .read_refresh_attempts = lane.metrics.read_refresh_attempts.load(std::memory_order_relaxed),
             .read_refresh_successes = lane.metrics.read_refresh_successes.load(std::memory_order_relaxed),
             .read_refresh_failures = lane.metrics.read_refresh_failures.load(std::memory_order_relaxed),
             .read_refresh_deferrals = lane.metrics.read_refresh_deferrals.load(std::memory_order_relaxed),
             .generations_retired = lane.generation.generations_retired.load(std::memory_order_relaxed),
             .shutdown_generations_reclaimed =
                 lane.generation.shutdown_generations_reclaimed.load(std::memory_order_relaxed),
             .generation_admission_backpressure_total =
                 lane.generation.generation_admission_backpressure_total.load(std::memory_order_relaxed),
             .reader_shutdown_finalized =
                 lane.generation.reader_shutdown_finalized.load(std::memory_order_relaxed),
             .retired_generation_count =
                 lane.generation.retired_generation_count.load(std::memory_order_relaxed),
             .delta_entries = lane.generation.delta_entries.load(std::memory_order_relaxed),
             .delta_record_versions = lane.generation.delta_record_versions.load(std::memory_order_relaxed),
             .delta_arena_record_bytes =
                 lane.generation.delta_arena_record_bytes.load(std::memory_order_relaxed),
             .delta_arena_key_bytes = lane.generation.delta_arena_key_bytes.load(std::memory_order_relaxed),
             .delta_arena_key_storage_bytes =
                 lane.generation.delta_arena_key_storage_bytes.load(std::memory_order_relaxed),
             .read_generation_memory = generation_memory,
             .read_merge_active = lane.merge.read_merge_active.load(std::memory_order_relaxed),
             .read_merge_post_entries = lane.merge.read_merge_post_entries.load(std::memory_order_relaxed),
             .read_merge_starts = lane.merge.read_merge_starts.load(std::memory_order_relaxed),
             .read_merge_completions = lane.merge.read_merge_completions.load(std::memory_order_relaxed),
             .read_merge_failures = lane.merge.read_merge_failures.load(std::memory_order_relaxed),
             .read_merge_backpressure = lane.merge.read_merge_backpressure.load(std::memory_order_relaxed),
             .read_merge_slots_processed =
                 lane.merge.read_merge_slots_processed.load(std::memory_order_relaxed),
             .read_merge_remaining_slots =
                 lane.merge.read_merge_remaining_slots.load(std::memory_order_relaxed),
             .read_merge_post_capacity_remaining =
                 lane.merge.read_merge_post_capacity_remaining.load(std::memory_order_relaxed),
             .maximum_read_merge_quantum_slots =
                 lane.merge.maximum_read_merge_quantum_slots.load(std::memory_order_relaxed),
             .sync_admitted = lane.sync.sync_admitted.load(std::memory_order_relaxed),
             .queue_wait_histogram = lane.metrics.queue_wait_histogram.snapshot(),
             .service_histogram = lane.metrics.service_histogram.snapshot()});
    }
    return result;
}

void ShardPairRuntime::note_writer_exit() noexcept {
    if (active_writers_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        active_writers_.notify_all();
    }
}

auto ShardPairRuntime::stop_and_drain(const std::optional<std::chrono::milliseconds> deadline) -> Status {
    GS_FAULT_SITE(close);
    if (!stopping_.exchange(true, std::memory_order_acq_rel)) {
        auto observed = admission_state_.fetch_or(kAdmissionClosed, std::memory_order_acq_rel);
        while ((observed & kAdmissionCountMask) != 0) {
            admission_state_.wait(observed, std::memory_order_acquire);
            observed = admission_state_.load(std::memory_order_acquire);
        }
        for (auto& lane : lanes_) {
            lane->async.stopping.store(true, std::memory_order_release);
            lane->async.signal.fetch_add(1U, std::memory_order_release);
            lane->async.signal.notify_one();
        }
    }

    bool timed_out = false;
    if (deadline.has_value()) {
        const auto deadline_at = std::chrono::steady_clock::now() + *deadline;
        while (active_writers_.load(std::memory_order_acquire) != 0 &&
               std::chrono::steady_clock::now() < deadline_at) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        timed_out = active_writers_.load(std::memory_order_acquire) != 0;
        if (timed_out) {
            abandon_queued_mutations();
        }
    }

    for (auto& lane : lanes_) {
        if (lane->thread.joinable()) {
            lane->thread.join();
        }
    }
    if (timed_out) {
        return fail(ErrorCode::unavailable, "shutdown drain deadline exceeded");
    }
    return {};
}

auto ShardPairRuntime::finalize_reader_shutdown() -> Status {
    const std::lock_guard shutdown_lock{reader_shutdown_mutex_};
    if (!stopping_.load(std::memory_order_acquire) || active_writers_.load(std::memory_order_acquire) != 0U) {
        return fail(ErrorCode::unavailable,
                    "paired Reader shutdown requires stopped mutation admission and joined Writers");
    }

    // Validate the complete set first: never partially finalize a multi-pair
    // runtime when an embedded counted Reader still owns a generation.
    for (const auto& lane : lanes_) {
        if (lane->reclaim.active_read_leases.load(std::memory_order_acquire) != 0U) {
            return fail(ErrorCode::unavailable, "paired Reader shutdown has an active read lease");
        }
    }

    // No Writer can resume after stop_and_drain() joins every dedicated thread,
    // and no embedded Reader lease remains after the validation above. An
    // unfinished incremental merge is Writer-private maintenance state, not
    // published authority; discard it before terminal generation reclaim. The
    // slot-pool merge cut owns a pin, so retaining it here would make an
    // otherwise quiescent shutdown permanently incomplete.
    for (auto& lane : lanes_) {
        lane->merge.read_merge.reset();
        lane->merge.merge_retry_blocked = false;
        store_merge_progress(lane->merge);
    }

    for (auto& lane : lanes_) {
        if (lane->generation.reader_shutdown_finalized.load(std::memory_order_acquire)) {
            continue;
        }
        if (auto status = finalize_generation_reader_shutdown(lane->generation); !status) {
            return status;
        }
    }
    return {};
}

void ShardPairRuntime::abandon_queued_mutations() noexcept {
    expire_remaining_.store(true, std::memory_order_release);
    for (auto& lane_ptr : lanes_) {
        auto& lane = *lane_ptr;
        lane.async.signal.fetch_add(1U, std::memory_order_release);
        lane.async.signal.notify_one();
        for (;;) {
            std::optional<runtime_detail::AsyncMutationTask> task;
            {
                const std::lock_guard lock{lane.async.queue_consumer_mutex};
                task = lane.queue.try_pop();
            }
            if (!task) {
                break;
            }
            lane.async.queued_bytes.fetch_sub(task->admission_bytes, std::memory_order_relaxed);
            lane.metrics.expired_before_store.fetch_add(1U, std::memory_order_relaxed);
            lane.metrics.completed.fetch_add(1U, std::memory_order_relaxed);
            MutationOutcome outcome{.context = task->context,
                                    .request_id = task->request_id,
                                    .admission_bytes = task->admission_bytes,
                                    .payload_slot = task->payload_slot,
                                    .writer_epoch =
                                        lane.generation.writer_epoch.load(std::memory_order_relaxed)};
            // This is a noexcept shutdown path and may itself run under memory
            // pressure. A code-only Error default-constructs without allocating;
            // building a diagnostic string here could otherwise terminate before
            // the accepted mutation receives its definitive rejection.
            static_assert(std::is_nothrow_default_constructible_v<Error>);
            outcome.error.emplace();
            outcome.error->code = ErrorCode::resource_exhausted;
            if (!runtime_detail::deliver_outcome(task->sink, std::move(outcome))) {
                std::terminate();
            }
            runtime_detail::notify_sink(task->sink);
        }
    }
}

void ShardPairRuntime::wake(Lane& lane) noexcept {
    lane.async.signal.fetch_add(1U, std::memory_order_release);
    lane.async.signal.notify_one();
}

void ShardPairRuntime::wait_sync_done(SyncMutation& node) noexcept {
    // Caller-side ladder mirrors Writer idle: pause → yield → park (Wave 2).
    for (unsigned spin = 0; spin < 32U; ++spin) {
        if (node.done.load(std::memory_order_acquire)) {
            return;
        }
#if defined(__aarch64__) || defined(_M_ARM64)
        __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }
    for (unsigned yield_spin = 0; yield_spin < 8U; ++yield_spin) {
        if (node.done.load(std::memory_order_acquire)) {
            return;
        }
        std::this_thread::yield();
    }
    node.done.wait(false, std::memory_order_acquire);
}

auto ShardPairRuntime::mutate(const std::size_t shard, const MutationKind kind, const HashedKey& key,
                              const std::span<const std::byte> value, const std::uint64_t expire_at_ns,
                              const ValueType type) -> Status {
    if (shard >= lanes_.size()) {
        return fail(ErrorCode::invalid_argument, "paired mutation shard is out of range");
    }
    {
        GS_PHASE_PUT(admit);
        if (!begin_submission()) {
            return fail(ErrorCode::unavailable, "paired runtime is not accepting mutations");
        }
    }

    struct AdmissionGuard final {
        ShardPairRuntime& runtime;

        ~AdmissionGuard() {
            runtime.finish_submission();
        }
    } admission{*this};

    // Mirror async try_submit: sticky fail-closed / stop must reject sync embeds too.
    // Otherwise volatile pairs keep mutating after publish_fail_closed (durable is masked
    // by DurableRuntime::healthy checks on the Store API).
    if (!healthy_.load(std::memory_order_acquire) || !started_.load(std::memory_order_acquire) ||
        stopping_.load(std::memory_order_acquire) ||
        lanes_[shard]->async.stopping.load(std::memory_order_acquire)) {
        return fail(ErrorCode::unavailable, "paired runtime is fail-closed");
    }

    SyncMutation node{.kind = kind, .type = type, .key = &key, .value = value, .expire_at_ns = expire_at_ns};
    auto& lane = *lanes_[shard];
    GS_FAULT_SITE(enqueue);
    {
        GS_PHASE_PUT(enqueue);
        const std::lock_guard lock{lane.sync.sync_mutex};
        node.next = lane.sync_head;
        lane.sync_head = &node;
    }
    lane.sync.sync_admitted.fetch_add(1U, std::memory_order_relaxed);
    if (combining_enabled()) {
        combine_sync_lane(shard);
        if (!node.done.load(std::memory_order_acquire)) {
            wake(lane);
        }
        GS_PHASE_PUT(ack);
        wait_sync_done(node);
        return node.status;
    }
    wake(lane);
    {
        GS_PHASE_PUT(ack);
        wait_sync_done(node);
    }
    return node.status;
}

auto ShardPairRuntime::mutate_batch(const std::size_t shard, const std::span<const SyncBatchItem> items,
                                    const std::span<Status> statuses) -> Status {
    if (shard >= lanes_.size()) {
        return fail(ErrorCode::invalid_argument, "paired mutation shard is out of range");
    }
    if (items.size() != statuses.size()) {
        return fail(ErrorCode::invalid_argument, "paired mutation batch status span size mismatch");
    }
    if (items.empty()) {
        return {};
    }
    for (const auto& item : items) {
        if (item.key == nullptr) {
            return fail(ErrorCode::invalid_argument, "paired mutation batch item has a null key");
        }
    }
    if (!begin_submission()) {
        for (auto& status : statuses) {
            status = Status{fail(ErrorCode::unavailable, "paired runtime is not accepting mutations")};
        }
        return fail(ErrorCode::unavailable, "paired runtime is not accepting mutations");
    }

    struct AdmissionGuard final {
        ShardPairRuntime& runtime;

        ~AdmissionGuard() {
            runtime.finish_submission();
        }
    } admission{*this};

    if (!healthy_.load(std::memory_order_acquire) || !started_.load(std::memory_order_acquire) ||
        stopping_.load(std::memory_order_acquire) ||
        lanes_[shard]->async.stopping.load(std::memory_order_acquire)) {
        for (auto& status : statuses) {
            status = Status{fail(ErrorCode::unavailable, "paired runtime is fail-closed")};
        }
        return fail(ErrorCode::unavailable, "paired runtime is fail-closed");
    }

    // Stack nodes for the common ≤32 path; heap for larger caller batches.
    constexpr std::size_t kStackBatch = 32U;
    std::array<SyncMutation, kStackBatch> stack_nodes{};
    std::unique_ptr<SyncMutation[]> heap_nodes;
    SyncMutation* nodes = stack_nodes.data();
    if (items.size() > kStackBatch) {
        heap_nodes = std::make_unique<SyncMutation[]>(items.size());
        nodes = heap_nodes.get();
    }
    for (std::size_t index = 0; index < items.size(); ++index) {
        nodes[index].kind = items[index].kind;
        nodes[index].key = items[index].key;
        nodes[index].value = items[index].value;
        nodes[index].expire_at_ns = items[index].expire_at_ns;
        nodes[index].status = {};
        nodes[index].done.store(false, std::memory_order_relaxed);
        nodes[index].next = nullptr;
    }

    auto& lane = *lanes_[shard];
    GS_FAULT_SITE(enqueue);
    {
        const std::lock_guard lock{lane.sync.sync_mutex};
        // Same LIFO push as single mutate; Writer reverses to FIFO.
        for (std::size_t index = 0; index < items.size(); ++index) {
            nodes[index].next = lane.sync_head;
            lane.sync_head = &nodes[index];
        }
    }
    lane.sync.sync_admitted.fetch_add(items.size(), std::memory_order_relaxed);
    if (combining_enabled()) {
        combine_sync_lane(shard);
        bool all_done = true;
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (!nodes[index].done.load(std::memory_order_acquire)) {
                all_done = false;
                break;
            }
        }
        if (!all_done) {
            wake(lane);
        }
    } else {
        wake(lane);
    }

    for (std::size_t index = 0; index < items.size(); ++index) {
        wait_sync_done(nodes[index]);
        statuses[index] = nodes[index].status;
    }
    return {};
}

ShardPairRuntime::ReadLease::ReadLease(const ShardPairRuntime& runtime, const std::size_t shard) noexcept {
    if (shard >= runtime.lanes_.size()) {
        return;
    }
    runtime_ = &runtime;
    shard_ = shard;
    auto& lane = *runtime.lanes_[shard];
    // Publish the lease before adopting the pointer. The fence pairs with the
    // Writer's reclamation fence and closes the load-then-increment window that
    // could otherwise free a generation while a Reader was adopting it.
    lane.reclaim.active_read_leases.fetch_add(1U, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    GS_FAULT_SITE(adopt);
    generation_ = load_published_generation(lane.generation);
    if (generation_ == nullptr) {
        lane.reclaim.active_read_leases.fetch_sub(1U, std::memory_order_release);
        runtime_ = nullptr;
    }
}

ShardPairRuntime::ReadLease::~ReadLease() {
    if (runtime_ == nullptr || generation_ == nullptr) {
        return;
    }
    auto& lane = *runtime_->lanes_[shard_];
    // Drop the lease without waking the Writer. Reclaim is opportunistic: the
    // Writer observes reclaim_requested on its next loop turn / park predicate.
    // Waking on every serial GET created a Reader→Writer ping-pong that dominated
    // embedded get_copy cost while contributing no correctness benefit (retired
    // generations are only produced by publication, which already wakes Writer).
    if (lane.reclaim.active_read_leases.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        lane.generation.reclaim_requested.store(true, std::memory_order_release);
    }
}

} // namespace glifistore::store::paired
