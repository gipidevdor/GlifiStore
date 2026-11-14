#include "glifistore/server/pair_writer.hpp"

#include "store/store_internal.hpp"

#include <utility>

namespace glifistore::server {
namespace {

[[nodiscard]] auto deliver_completion(void* completions, store::paired::MutationOutcome outcome) noexcept
    -> bool {
    auto* queue = static_cast<BoundedSpscQueue<MutationCompletion>*>(completions);
    MutationCompletion completion{.connection = ConnectionToken::decode(outcome.context.value),
                                  .request_id = outcome.request_id,
                                  .admission_bytes = outcome.admission_bytes,
                                  .payload_slot = outcome.payload_slot,
                                  .writer_epoch = outcome.writer_epoch,
                                  .error = std::move(outcome.error)};
    return queue->try_push(std::move(completion));
}

void notify_wakeup(void* wakeup) noexcept {
    static_cast<void>(static_cast<Wakeup*>(wakeup)->notify());
}

} // namespace

PairWriterPool::PairWriterPool(store::paired::ShardPairRuntime& runtime) noexcept : runtime_(runtime) {}

auto PairWriterPool::create(Store& store, const std::size_t worker_count,
                            const std::size_t capacity_per_worker, const std::size_t payload_bytes_per_worker,
                            const std::chrono::milliseconds maximum_queue_wait,
                            const PairReadMergeConfig read_merge)
    -> Result<std::unique_ptr<PairWriterPool>> try {
    auto* runtime = detail::StoreAccess::shard_pair_runtime(store);
    if (runtime == nullptr) {
        return fail(ErrorCode::invalid_argument,
                    "glifistored requires a paired Store; open with StoreConcurrencyMode::paired");
    }
    if (worker_count == 0 || worker_count != store.worker_count() || worker_count != runtime->shard_count() ||
        capacity_per_worker == 0 || payload_bytes_per_worker == 0 || read_merge.delta_entries == 0 ||
        read_merge.maximum_post_entries == 0 || read_merge.quantum_slots == 0 ||
        read_merge.delta_entries > PairReadGeneration::kMaximumIncrementalDeltaEntries ||
        read_merge.maximum_post_entries >
            PairReadGeneration::kMaximumIncrementalDeltaEntries - read_merge.delta_entries) {
        return fail(ErrorCode::invalid_argument,
                    "paired mutation executor requires exactly one Writer per nonempty Store shard");
    }
    if (!runtime->async_lane_enabled()) {
        return fail(ErrorCode::invalid_argument,
                    "paired Store was opened without an asynchronous mutation lane; set "
                    "StoreConfig::paired.async_lane_capacity and async_lane_payload_bytes");
    }
    static_cast<void>(maximum_queue_wait);
    static_cast<void>(capacity_per_worker);
    static_cast<void>(payload_bytes_per_worker);
    return std::unique_ptr<PairWriterPool>(new PairWriterPool(*runtime));
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "paired mutation executor allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "paired mutation executor construction failed");
}

auto PairWriterPool::start() -> Status {
    // Store::open already started the shared ShardPairRuntime.
    return {};
}

auto PairWriterPool::mutation_admission_bytes(const std::size_t key_bytes,
                                              const std::size_t value_bytes) noexcept
    -> std::optional<std::size_t> {
    return store::paired::ShardPairRuntime::mutation_admission_bytes(key_bytes, value_bytes);
}

auto PairWriterPool::try_submit(const MutationRequest& request) noexcept -> std::optional<std::size_t> {
    if (request.completions == nullptr || request.wakeup == nullptr) {
        return std::nullopt;
    }
    store::paired::AsyncMutationRequest async{
        .shard = request.worker_index,
        .kind = request.kind == MutationKind::put ? store::paired::MutationKind::put
                                                  : store::paired::MutationKind::erase,
        .context = {.value = request.connection.encode()},
        .request_id = request.request_id,
        .key = request.key,
        .key_hash = request.key_hash,
        .value = request.value,
        .expire_at_ns = request.expire_at_ns,
        .sink = {.completions = request.completions,
                 .wakeup = request.wakeup,
                 .deliver = &deliver_completion,
                 .notify = &notify_wakeup},
    };
    return runtime_.try_submit(async);
}

auto PairWriterPool::release_payload(const std::size_t worker_index,
                                     const std::uint32_t payload_slot) noexcept -> bool {
    return runtime_.release_payload(worker_index, payload_slot);
}

void PairWriterPool::note_rejected(const std::size_t worker_index) noexcept {
    runtime_.note_rejected(worker_index);
}

auto PairWriterPool::stats() const -> std::vector<PairWriterStats> {
    auto raw = runtime_.stats();
    std::vector<PairWriterStats> out;
    out.reserve(raw.size());
    for (const auto& lane : raw) {
        out.push_back(PairWriterStats{
            .worker_index = lane.worker_index,
            .reader_safe_epoch = lane.reader_safe_epoch,
            .writer_epoch = lane.writer_epoch,
            .queue_depth = lane.queue_depth,
            .queued_bytes = lane.queued_bytes,
            .maximum_queue_depth = lane.maximum_queue_depth,
            .maximum_queued_bytes = lane.maximum_queued_bytes,
            .payload_slot_capacity = lane.payload_slot_capacity,
            .payload_slots_in_use = lane.payload_slots_in_use,
            .maximum_payload_slots_in_use = lane.maximum_payload_slots_in_use,
            .payload_arena_capacity_bytes = lane.payload_arena_capacity_bytes,
            .payload_arena_storage_bytes = lane.payload_arena_storage_bytes,
            .payload_arena_bytes_in_use = lane.payload_arena_bytes_in_use,
            .maximum_payload_arena_bytes_in_use = lane.maximum_payload_arena_bytes_in_use,
            .payload_admission_bytes_in_use = lane.payload_admission_bytes_in_use,
            .maximum_payload_admission_bytes_in_use = lane.maximum_payload_admission_bytes_in_use,
            .payload_slot_full_total = lane.payload_slot_full_total,
            .payload_arena_full_total = lane.payload_arena_full_total,
            .payload_too_large_total = lane.payload_too_large_total,
            .admitted = lane.admitted,
            .rejected = lane.rejected,
            .expired_before_store = lane.expired_before_store,
            .completed = lane.completed,
            .conflict_retries = lane.conflict_retries,
            .conflict_retry_commits = lane.conflict_retry_commits,
            .writer_batches = lane.writer_batches,
            .writer_batch_records = lane.writer_batch_records,
            .maximum_writer_batch_records = lane.maximum_writer_batch_records,
            .total_writer_batch_wait_ns = lane.total_writer_batch_wait_ns,
            .maximum_writer_batch_wait_ns = lane.maximum_writer_batch_wait_ns,
            .writer_batch_durability_deadline_closes = lane.writer_batch_durability_deadline_closes,
            .writer_batch_queue_deadline_closes = lane.writer_batch_queue_deadline_closes,
            .sync_drain_turns = lane.sync_drain_turns,
            .sync_turn_splits = lane.sync_turn_splits,
            .sync_async_fairness_turns = lane.sync_async_fairness_turns,
            .publications = lane.publications,
            .publication_records = lane.publication_records,
            .completion_notifications = lane.completion_notifications,
            .total_queue_wait_ns = lane.total_queue_wait_ns,
            .maximum_queue_wait_ns = lane.maximum_queue_wait_ns,
            .total_service_ns = lane.total_service_ns,
            .maximum_service_ns = lane.maximum_service_ns,
            .read_catalog_revision = lane.read_catalog_revision,
            .read_refresh_attempts = lane.read_refresh_attempts,
            .read_refresh_successes = lane.read_refresh_successes,
            .read_refresh_failures = lane.read_refresh_failures,
            .read_refresh_deferrals = lane.read_refresh_deferrals,
            .generations_retired = lane.generations_retired,
            .shutdown_generations_reclaimed = lane.shutdown_generations_reclaimed,
            .generation_admission_backpressure_total = lane.generation_admission_backpressure_total,
            .reader_shutdown_finalized = lane.reader_shutdown_finalized,
            .retired_generation_count = lane.retired_generation_count,
            .delta_entries = lane.delta_entries,
            .delta_record_versions = lane.delta_record_versions,
            .delta_arena_record_bytes = lane.delta_arena_record_bytes,
            .delta_arena_key_bytes = lane.delta_arena_key_bytes,
            .delta_arena_key_storage_bytes = lane.delta_arena_key_storage_bytes,
            .read_generation_memory = lane.read_generation_memory,
            .read_merge_active = lane.read_merge_active,
            .read_merge_post_entries = lane.read_merge_post_entries,
            .read_merge_starts = lane.read_merge_starts,
            .read_merge_completions = lane.read_merge_completions,
            .read_merge_failures = lane.read_merge_failures,
            .read_merge_backpressure = lane.read_merge_backpressure,
            .read_merge_slots_processed = lane.read_merge_slots_processed,
            .read_merge_remaining_slots = lane.read_merge_remaining_slots,
            .read_merge_post_capacity_remaining = lane.read_merge_post_capacity_remaining,
            .maximum_read_merge_quantum_slots = lane.maximum_read_merge_quantum_slots,
            .queue_wait_histogram = lane.queue_wait_histogram,
            .service_histogram = lane.service_histogram,
        });
    }
    return out;
}

auto PairWriterPool::adopt_read_generation(const std::size_t worker_index,
                                           const std::uint64_t minimum_leased_epoch) const noexcept
    -> const PairReadGeneration* {
    return runtime_.adopt_read_generation(worker_index, minimum_leased_epoch);
}

void PairWriterPool::request_read_refresh(const std::size_t worker_index) noexcept {
    runtime_.request_read_refresh(worker_index);
}

auto PairWriterPool::healthy() const noexcept -> bool {
    return runtime_.healthy();
}

void PairWriterPool::abandon_queued_mutations() noexcept {
    runtime_.abandon_queued_mutations();
}

auto PairWriterPool::stop_and_drain(const std::optional<std::chrono::milliseconds> deadline) -> Status {
    // Store::close drains the shared runtime. Daemon stop may call this while the Store is still
    // open; draining here is idempotent with respect to an already-stopped runtime.
    return runtime_.stop_and_drain(deadline);
}

} // namespace glifistore::server
