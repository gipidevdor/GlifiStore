#include "glifistore/persistence/runtime_catalog.hpp"

#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/integer_math.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/swiss_table.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/durable_flush_coordinator.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "persistence/adaptive_batch_sizer.hpp"
#include "persistence/blob_storage.hpp"
#include "persistence/hot_record_table.hpp"
#include "persistence/runtime_catalog_detail.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace glifistore {
using namespace glifistore::runtime_catalog_detail;

auto DurableRuntimeCatalog::PublishedReadPin::matches(const RecordRef& reference) const noexcept -> bool {
    return generation_ && generation_->identity.segment_id == reference.segment_id &&
           generation_->identity.generation == reference.generation;
}

[[nodiscard]] auto DurableRuntimeCatalog::RuntimeWorker::hot_cache_total_bytes() const noexcept
    -> std::uint64_t {
    const auto table_bytes = hot_cache_table_bytes(hot_records.capacity());
    const auto first =
        hot_record_resident_bytes > std::numeric_limits<std::uint64_t>::max() - hot_record_staged_bytes
            ? std::numeric_limits<std::uint64_t>::max()
            : hot_record_resident_bytes + hot_record_staged_bytes;
    return first > std::numeric_limits<std::uint64_t>::max() - table_bytes
               ? std::numeric_limits<std::uint64_t>::max()
               : first + table_bytes;
}

void DurableRuntimeCatalog::RuntimeWorker::erase_hot_record(const std::string_view key,
                                                            const std::uint64_t key_hash) noexcept {
    if (auto* existing = hot_records.find(key, key_hash); existing != nullptr) {
        subtract_hot_record_accounting(hot_record_resident_bytes, key, *existing);
        static_cast<void>(hot_records.erase(key, key_hash));
        get_path_metrics.hot_evictions.fetch_add(1U, std::memory_order_relaxed);
    }
}

auto DurableRuntimeCatalog::RuntimeWorker::drain_deferred_ttl(const std::size_t limit) -> Status {
    if (compaction_index_frozen) {
        return {};
    }
    if (glifistore::fault::consume_fail(glifistore::fault::Site::deferred_ttl)) {
        return fail(ErrorCode::resource_exhausted, "injected deferred TTL drain failure");
    }
    std::size_t processed = 0;
    while (processed < limit && !deferred_ttl_reclaims.empty()) {
        auto pending = std::move(deferred_ttl_reclaims.front());
        deferred_ttl_reclaims.erase(deferred_ttl_reclaims.begin());
        ++processed;
        const HashedKey key{.key = pending.key, .hash = pending.key_hash};
        const auto current = index.find(key);
        if (!current || *current != pending.reference) {
            ++deferred_ttl_skipped;
            continue;
        }
        const auto erased = index.erase_no_compact(key);
        record_compaction_change(key, std::nullopt);
        if (auto counted = update_live_record_bytes(erased.previous, std::nullopt); !counted) {
            return counted;
        }
        erase_hot_record(key);
        ++deferred_ttl_applied;
    }
    return {};
}

auto DurableRuntimeCatalog::RuntimeWorker::defer_or_reclaim_expired(const HashedKey& key,
                                                                    const RecordRef& reference,
                                                                    const std::size_t backlog_limit)
    -> Status {
    // Drop any matching hot row immediately so the expired value cannot be
    // served; Index removal may wait for a bounded Worker drain.
    erase_hot_record(key);
    get_path_metrics.expired_ttl_gets.fetch_add(1U, std::memory_order_relaxed);
    if (compaction_index_frozen) {
        return {};
    }

    if (backlog_limit == 0) {
        const auto current = index.find(key);
        if (!current || *current != reference) {
            ++deferred_ttl_skipped;
            return {};
        }
        const auto erased = index.erase_no_compact(key);
        record_compaction_change(key, std::nullopt);
        if (auto counted = update_live_record_bytes(erased.previous, std::nullopt); !counted) {
            return counted;
        }
        ++deferred_ttl_applied;
        return {};
    }

    if (deferred_ttl_reclaims.size() >= backlog_limit) {
        if (auto drained = drain_deferred_ttl(std::max<std::size_t>(1, backlog_limit / 8U)); !drained) {
            return drained;
        }
    }
    if (deferred_ttl_reclaims.size() >= backlog_limit) {
        // Backlog still full: reclaim this exact reference synchronously.
        const auto current = index.find(key);
        if (!current || *current != reference) {
            ++deferred_ttl_skipped;
            return {};
        }
        const auto erased = index.erase_no_compact(key);
        record_compaction_change(key, std::nullopt);
        if (auto counted = update_live_record_bytes(erased.previous, std::nullopt); !counted) {
            return counted;
        }
        ++deferred_ttl_applied;
        return {};
    }

    deferred_ttl_reclaims.push_back(
        DeferredTtlReclaim{.key = std::string{key.key}, .key_hash = key.hash, .reference = reference});
    ++deferred_ttl_enqueued;
    return {};
}

[[nodiscard]] auto DurableRuntimeCatalog::RuntimeWorker::prepare_hot_record(
    const std::size_t worker_index, const std::size_t worker_count, const DurableResourceLimits& limits,
    const std::string_view key, const std::uint64_t key_hash, const std::span<const std::byte> value,
    const std::uint64_t expire_at_ns, const std::uint64_t publication_staging_bytes)
    -> Result<PreparedHotRecord> {
    if (!limits.hot_cache_enabled) {
        get_path_metrics.admission_bypasses.fetch_add(1U, std::memory_order_relaxed);
        return PreparedHotRecord{};
    }
    if (limits.max_hot_cache_value_bytes == 0 ||
        value.size() > static_cast<std::size_t>(limits.max_hot_cache_value_bytes)) {
        get_path_metrics.size_rejected.fetch_add(1U, std::memory_order_relaxed);
        get_path_metrics.admission_bypasses.fetch_add(1U, std::memory_order_relaxed);
        return PreparedHotRecord{};
    }
    auto resident_charge = hot_record_accounted_bytes(key.size(), value.size());
    if (!resident_charge) {
        return unexpected(resident_charge.error());
    }
    const auto slot_charge = detail::hot_record_slot_bytes();
    if (*resident_charge > std::numeric_limits<std::uint64_t>::max() - slot_charge) {
        return fail(ErrorCode::arithmetic_overflow, "hot-cache staging slot overflow");
    }
    const auto staged_resident_charge = slot_charge + *resident_charge;
    if (publication_staging_bytes > std::numeric_limits<std::uint64_t>::max() - staged_resident_charge) {
        return fail(ErrorCode::arithmetic_overflow, "hot-cache publication staging overflow");
    }
    const auto staged_charge = staged_resident_charge + publication_staging_bytes;
    const auto budget = hot_cache_worker_budget(worker_index, worker_count, limits);
    if (hot_records.size() > std::numeric_limits<std::size_t>::max() - hot_record_staged_entries) {
        return fail(ErrorCode::arithmetic_overflow, "hot-cache entry accounting overflow");
    }
    const auto projected_entries = hot_records.size() + hot_record_staged_entries;
    const bool entry_exhausted = limits.max_hot_cache_entries_per_worker == 0 ||
                                 projected_entries >= limits.max_hot_cache_entries_per_worker;
    const bool staging_exhausted =
        limits.max_hot_cache_staging_bytes_per_worker == 0 ||
        hot_record_staged_bytes > limits.max_hot_cache_staging_bytes_per_worker ||
        staged_charge > limits.max_hot_cache_staging_bytes_per_worker - hot_record_staged_bytes;
    if (budget == 0 || entry_exhausted || staging_exhausted) {
        get_path_metrics.admission_bypasses.fetch_add(1U, std::memory_order_relaxed);
        return PreparedHotRecord{};
    }

    if (hot_record_staged_entries == std::numeric_limits<std::size_t>::max() ||
        projected_entries == std::numeric_limits<std::size_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "hot-cache staged entry accounting overflow");
    }
    const auto additional = hot_record_staged_entries + 1U;
    const auto plan = detail::plan_hot_record_reserve(hot_records.size(), additional, hot_records.capacity());
    if (plan.overflow) {
        return fail(ErrorCode::arithmetic_overflow, "hot Record publication capacity overflow");
    }
    const auto projected_capacity = plan.target == 0 ? hot_records.capacity() : plan.target;
    const auto projected_table = hot_cache_table_bytes(projected_capacity);
    const auto fixed =
        hot_record_resident_bytes > std::numeric_limits<std::uint64_t>::max() - hot_record_staged_bytes
            ? std::numeric_limits<std::uint64_t>::max()
            : hot_record_resident_bytes + hot_record_staged_bytes;
    const auto available = fixed >= budget ? 0 : budget - fixed;
    if (projected_table > available || staged_charge > available - projected_table) {
        get_path_metrics.admission_bypasses.fetch_add(1U, std::memory_order_relaxed);
        return PreparedHotRecord{};
    }
    if (plan.target != 0) {
        const auto target_entries = plan.target - plan.target / HotRecordTable::kLoadDenominator;
        if (auto reserved = hot_records.reserve(target_entries); !reserved) {
            return unexpected(reserved.error());
        }
    }
    const auto after_reserve = hot_cache_total_bytes();
    if (after_reserve >= budget || staged_charge > budget - after_reserve) {
        get_path_metrics.admission_bypasses.fetch_add(1U, std::memory_order_relaxed);
        return PreparedHotRecord{};
    }

    HotRecordEntry staged_entry{.value_size = value.size(), .expire_at_ns = expire_at_ns};
    if (!value.empty() && value.size() <= HotRecordEntry::kInlineValueBytes) {
        std::copy(value.begin(), value.end(), staged_entry.inline_value);
    } else if (!value.empty()) {
        auto mutable_value = std::make_shared<std::byte[]>(value.size());
        std::copy(value.begin(), value.end(), mutable_value.get());
        staged_entry.heap_value = std::move(mutable_value);
    }
    hot_record_staged_bytes += staged_charge;
    ++hot_record_staged_entries;
    return PreparedHotRecord{
        std::string{key},           key_hash,      std::move(staged_entry), &hot_record_staged_bytes,
        &hot_record_staged_entries, staged_charge, *resident_charge};
}

[[nodiscard]] auto DurableRuntimeCatalog::RuntimeWorker::publish_hot_record(PreparedHotRecord& prepared,
                                                                            const RecordRef& reference)
    -> Status {
    if (prepared.empty()) {
        return {};
    }
    prepared.mapped().reference = reference;
    const auto charge = prepared.resident_charge();
    const auto key_hash = prepared.key_hash();
    if (auto* existing = hot_records.find(prepared.key(), key_hash); existing != nullptr) {
        subtract_hot_record_accounting(hot_record_resident_bytes, prepared.key(), *existing);
        *existing = prepared.take_entry();
    } else {
        auto key = prepared.take_key();
        auto entry = prepared.take_entry();
        if (auto inserted = hot_records.insert_or_assign(std::move(key), key_hash, std::move(entry));
            !inserted) {
            return unexpected(inserted.error());
        }
    }
    hot_record_resident_bytes += charge;
    return {};
}

DurableRuntimeCatalog::DurableRuntimeCatalog(DataDirectory directory, DurableRecoveryState recovered,
                                             DurableRuntimeOptions options)
    : directory_(std::move(directory)), worker_routing_(recovered.manifest.worker_routing()),
      manifest_(std::move(recovered.manifest)), namespace_audit_(std::move(recovered.namespace_audit)),
      segments_(std::move(recovered.segments)), recovery_stats_(recovered.stats), options_(options) {
    workers_.reserve(recovered.workers.size());
    for (auto& worker : recovered.workers) {
        auto runtime_worker = std::make_unique<RuntimeWorker>(std::move(worker));
        if (options_.batch) {
            runtime_worker->batch_sizer.reset(options_.batch->min_records, options_.batch->max_records);
            runtime_worker->pending_group_mutations.reserve(options_.batch->max_records);
            runtime_worker->batch_metrics.current_record_target.store(runtime_worker->batch_sizer.target(),
                                                                      std::memory_order_relaxed);
        }
        workers_.push_back(std::move(runtime_worker));
    }
    // A single Worker has one file-backed commit domain, so its dedicated
    // executor can own batch closure without cross-Worker coordination.
    dedicated_commit_executor_ = options_.strict_ack && options_.batch.has_value() && workers_.size() == 1;
    if (options_.commit_sync == SegmentCommitSync::deferred || options_.batch) {
        const bool periodic_sync = options_.commit_sync == SegmentCommitSync::deferred;
        const bool batch_timer = options_.batch.has_value();
        const auto batch_wait = options_.batch ? options_.batch->max_wait_ms : 0U;
        flusher_ = std::make_unique<DurableFlushCoordinator>(
            options_.sync_interval_ms, batch_wait, periodic_sync, batch_timer,
            [this](const bool force_all) -> Status {
                try {
                    if (force_all) {
                        auto flushed = flush_pending_batches(SegmentCommitSync::immediate);
                        if (!flushed) {
                            return flushed;
                        }
                        return flush_dirty_segments();
                    }
                    if (options_.strict_ack) {
                        return flush_due_batches(SegmentCommitSync::immediate);
                    }
                    auto flushed = options_.batch ? flush_due_batches(SegmentCommitSync::deferred)
                                                  : flush_pending_batches(SegmentCommitSync::deferred);
                    if (!flushed) {
                        return flushed;
                    }
                    return flush_dirty_segments();
                } catch (...) {
                    abandon_pending_batches();
                    throw;
                }
            });
    }
}

DurableRuntimeCatalog::~DurableRuntimeCatalog() {
    try {
        static_cast<void>(close());
    } catch (...) {
        healthy_.store(false, std::memory_order_release);
    }
}

auto DurableRuntimeCatalog::open_existing(const std::filesystem::path& path,
                                          const std::uint64_t recovery_now_ns, const FilesystemHooks hooks)
    -> Result<std::unique_ptr<DurableRuntimeCatalog>> {
    auto directory = DataDirectory::open_and_lock(path, hooks);
    if (!directory) {
        return unexpected(directory.error());
    }
    return open_locked(std::move(*directory), recovery_now_ns);
}

auto DurableRuntimeCatalog::open_locked(DataDirectory directory, const std::uint64_t recovery_now_ns,
                                        const DurableRuntimeOptions options)
    -> Result<std::unique_ptr<DurableRuntimeCatalog>> {
    if (auto valid = validate_durable_resource_limits(options.limits); !valid) {
        return unexpected(valid.error());
    }
    auto format_manifest = directory.read_manifest(options.limits.max_manifest_bytes);
    if (!format_manifest) {
        return unexpected(format_manifest.error());
    }
    if ((format_manifest->format_version == 2) != static_cast<bool>(options.blob_storage)) {
        return fail(ErrorCode::invalid_argument, "blob Store requires explicit embedded profile");
    }
    auto compaction = resolve_interrupted_compaction(directory, recovery_now_ns, options.limits);
    if (!compaction) {
        return unexpected(compaction.error());
    }
    std::optional<DurableRecoveryState> recovered = std::move(*compaction);
    if (!recovered) {
        if (auto completed = complete_interrupted_rotation(directory, options.limits); !completed) {
            return unexpected(completed.error());
        }
        auto ordinary = recover_durable_state(directory, recovery_now_ns, options.limits);
        if (!ordinary) {
            return unexpected(ordinary.error());
        }
        recovered.emplace(std::move(*ordinary));
    }
    if (recovered->segments.size() != recovered->manifest.segments.size() ||
        recovered->workers.size() != recovered->manifest.worker_count) {
        return fail(ErrorCode::corrupted_data, "durable recovery result is not aligned with its manifest");
    }
    if (std::ranges::any_of(recovered->workers, [](const RecoveredWorkerState& worker) {
            return worker.active_requires_rotation;
        })) {
        return fail(ErrorCode::unavailable,
                    "durable Store requires interrupted rotation completion before runtime service");
    }
    if (options.blob_storage) {
        auto roots = detail::verify_blob_closure(directory, recovered->manifest, *options.blob_storage);
        if (!roots) {
            return unexpected(roots.error());
        }
        auto cleaned = options.blob_storage->collect(*roots);
        if (!cleaned) {
            return unexpected(cleaned.error());
        }
    }
    auto runtime = std::unique_ptr<DurableRuntimeCatalog>(
        new DurableRuntimeCatalog(std::move(directory), std::move(*recovered), options));
    if (auto pinned = runtime->initialize_generation_pins(); !pinned) {
        return unexpected(pinned.error());
    }
    return runtime;
}

auto DurableRuntimeCatalog::initialize_generation_pins() -> Status {
    if (manifest_.segments.size() != segments_.size()) {
        return fail(ErrorCode::corrupted_data, "runtime Segment catalog is not aligned");
    }
    std::vector<std::shared_ptr<const RuntimeSegmentGeneration>> pins;
    pins.reserve(manifest_.segments.size());
    for (std::size_t index = 0; index < manifest_.segments.size(); ++index) {
        const auto& entry = manifest_.segments[index];
        const SegmentHeaderIdentity identity{.store_id = manifest_.store_id,
                                             .segment_id = entry.segment_id,
                                             .generation = entry.generation,
                                             .owner_worker = entry.owner_worker,
                                             .format_version = manifest_.format_version};
        auto opened = DurableSegmentFile::open(directory_, identity, SegmentFileOpenMode::read_only);
        if (!opened) {
            return unexpected(opened.error());
        }
        if (opened->selected_commit() != segments_[index].selected) {
            return fail(ErrorCode::corrupted_data,
                        "runtime Segment commit boundary changed while generation pins were built");
        }
        pins.push_back(std::make_shared<const RuntimeSegmentGeneration>(RuntimeSegmentGeneration{
            .identity = identity, .selected = segments_[index].selected, .file = std::move(*opened)}));
    }
    auto pin_slots = prepare_pin_slot_index(manifest_);
    if (!pin_slots) {
        return unexpected(pin_slots.error());
    }
    generation_pins_ = std::move(pins);
    pin_slot_by_segment_id_ = std::move(*pin_slots);
    return {};
}

auto DurableRuntimeCatalog::prepare_pin_slot_index(const Manifest& manifest)
    -> Result<std::vector<std::uint32_t>> {
    constexpr auto kAbsent = std::numeric_limits<std::uint32_t>::max();
    const auto needed = std::max(manifest.next_segment_id.value, static_cast<std::uint64_t>(1));
    if (needed > std::numeric_limits<std::size_t>::max() ||
        manifest.segments.size() >= static_cast<std::size_t>(kAbsent)) {
        return fail(ErrorCode::resource_exhausted, "Segment pin-slot index exceeds runtime addressability");
    }
    std::vector<std::uint32_t> slots(static_cast<std::size_t>(needed), kAbsent);
    for (std::size_t index = 0; index < manifest.segments.size(); ++index) {
        const auto segment_id = manifest.segments[index].segment_id.value;
        if (segment_id >= slots.size()) {
            return fail(ErrorCode::corrupted_data,
                        "Manifest Segment ID is outside its next-ID pin-slot boundary");
        }
        slots[static_cast<std::size_t>(segment_id)] = static_cast<std::uint32_t>(index);
    }
    return slots;
}

auto DurableRuntimeCatalog::catalog_index_for_segment(const SegmentId segment_id) const noexcept
    -> std::optional<std::size_t> {
    constexpr auto kAbsent = std::numeric_limits<std::uint32_t>::max();
    if (segment_id.value >= pin_slot_by_segment_id_.size()) {
        return std::nullopt;
    }
    const auto slot = pin_slot_by_segment_id_[static_cast<std::size_t>(segment_id.value)];
    if (slot == kAbsent || slot >= manifest_.segments.size() || slot >= generation_pins_.size()) {
        return std::nullopt;
    }
    if (manifest_.segments[slot].segment_id != segment_id) {
        return std::nullopt;
    }
    return slot;
}

auto DurableRuntimeCatalog::sync_worker_file(RuntimeWorker& worker, std::unique_lock<std::mutex>& worker_lock,
                                             std::shared_lock<std::shared_mutex>& catalog_lock) -> Status {
    if (!worker.cached_file || !worker.cached_writable || !worker.cached_file->is_dirty()) {
        return {};
    }
    if (!worker_lock.owns_lock() || !catalog_lock.owns_lock() || worker.mutation_io_active) {
        return fail(ErrorCode::internal_error, "Segment sync requires an unreserved Worker and catalog lock");
    }
    const auto identity = worker.cached_file->identity();
    const auto expected_selected = worker.cached_file->selected_commit();
    const auto pin_index = catalog_index_for_segment(identity.segment_id);
    if (!pin_index || !generation_pins_[*pin_index] || generation_pins_[*pin_index]->identity != identity) {
        return fail(ErrorCode::corrupted_data, "dirty Segment has no exact generation pin");
    }
    const auto generation_pin = generation_pins_[*pin_index];
    auto io_file = std::move(worker.cached_file);
    worker.cached_file.reset();
    worker.cached_writable = false;
    worker.mutation_io_active = true;
    catalog_lock.unlock();
    worker_lock.unlock();

    SegmentCommitResult synced;
    try {
        synced = io_file->sync_file();
    } catch (const std::bad_alloc&) {
        synced = {.outcome = SegmentCommitOutcome::indeterminate,
                  .error = Error{ErrorCode::resource_exhausted, {}}};
    } catch (...) {
        synced = {.outcome = SegmentCommitOutcome::indeterminate,
                  .error = Error{ErrorCode::internal_error, {}}};
    }

    worker_lock.lock();
    catalog_lock.lock();
    const auto current_position =
        std::lower_bound(manifest_.segments.begin(), manifest_.segments.end(), identity.segment_id,
                         [](const ManifestSegmentEntry& entry, const SegmentId id) {
                             return entry.segment_id.value < id.value;
                         });
    const auto current_pin_index = catalog_index_for_segment(identity.segment_id);
    const bool reservation_valid =
        current_position != manifest_.segments.end() && current_position->segment_id == identity.segment_id &&
        current_position->generation == identity.generation &&
        current_position->owner_worker == identity.owner_worker &&
        current_position->role == ManifestSegmentRole::active &&
        worker.active_segment == identity.segment_id && worker.mutation_io_active &&
        current_pin_index.has_value() && generation_pins_[*current_pin_index] == generation_pin &&
        io_file->identity() == identity &&
        expected_selected ==
            segments_[static_cast<std::size_t>(current_position - manifest_.segments.begin())].selected;
    worker.cached_file.emplace(std::move(*io_file));
    worker.cached_writable = true;
    worker.mutation_io_active = false;
    worker.mutation_io_finished.notify_all();
    if (!reservation_valid) {
        healthy_.store(false, std::memory_order_release);
        return fail(ErrorCode::corrupted_data, "Segment sync I/O reservation failed relinearization");
    }
    if (!synced.committed()) {
        healthy_.store(false, std::memory_order_release);
        return unexpected(synced.error.value_or(Error{ErrorCode::io_error, "Segment flush failed"}));
    }
    return {};
}

auto DurableRuntimeCatalog::flush_dirty_segments() -> Status {
    for (auto& worker : workers_) {
        // Exclusive durable_sync (no flusher): mutate elides worker.mutex while moving
        // cached_file and toggling mutation_io_active. Mutex+CV alone can lose the
        // wakeup or race the handle. ExclusiveIndexQuiesce arms the gate and drains
        // hot_path_depth first (do not hold mutex across the wait).
        const bool elide_worker_mutex = options_.exclusive_writer && flusher_ == nullptr;
        ExclusiveIndexQuiesce index_quiesce{*worker, elide_worker_mutex};
        std::unique_lock lock{worker->mutex};
        worker->mutation_io_finished.wait(
            lock, [&] { return !worker->mutation_io_active || !healthy_.load(std::memory_order_acquire); });
        if (!elide_worker_mutex) {
            // Non-elided path: wait for other Index quiesce holders (compact/GET).
            // When we hold ExclusiveIndexQuiesce, compaction_commit_active is true —
            // waiting for it clear would self-deadlock.
            worker->compaction_commit_finished.wait(
                lock, [&] { return !worker->compaction_commit_active.load(std::memory_order_relaxed); });
        }
        if (!healthy_.load(std::memory_order_acquire)) {
            return {};
        }
        auto& cached_file = worker->cached_file;
        if (!cached_file || !worker->cached_writable) {
            continue;
        }
        if (!cached_file.value().is_dirty()) {
            continue;
        }
        std::shared_lock catalog_lock{catalog_mutex_};
        if (auto synced = sync_worker_file(*worker, lock, catalog_lock); !synced) {
            return synced;
        }
    }
    return {};
}

auto DurableRuntimeCatalog::flush() -> Status {
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }
    if (dedicated_commit_executor_ && flusher_) {
        return flusher_->flush_all_blocking();
    }
    if (options_.batch || options_.commit_sync == SegmentCommitSync::deferred) {
        if (auto flushed = flush_pending_batches(SegmentCommitSync::immediate); !flushed) {
            return flushed;
        }
    }
    return flush_dirty_segments();
}

auto DurableRuntimeCatalog::backup_to(const std::filesystem::path& destination, const bool scan_records,
                                      const DurableResourceLimits& limits)
    -> Result<DurableStoreBackupReport> {
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }
    const auto copy_started = std::chrono::steady_clock::now();
    if (auto flushed = flush(); !flushed) {
        return unexpected(flushed.error());
    }
    // Exclusive catalog lock blocks rotation/compaction publication for the copy window. Callers must
    // already have fenced Store admissions so no mutation holds a worker lock waiting on shared
    // catalog (which would deadlock with this unique lock). Source CRC and destination verify are
    // deferred: structural source check only under the fence; Store::backup_to CRC-scans the
    // destination after admissions resume (copied bytes are the promotion gate).
    auto copied = [&]() -> Result<DurableStoreBackupReport> {
        const std::unique_lock catalog_lock{catalog_mutex_};
        if (!healthy()) {
            return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
        }
        const Manifest catalog_snapshot = manifest_;
        return backup_durable_store_from_open_directory(directory_, catalog_snapshot, destination,
                                                        scan_records, limits, /*verify_destination=*/false,
                                                        /*scan_source_records=*/false);
    }();
    if (!copied) {
        return unexpected(copied.error());
    }
    copied->catalog_copy_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - copy_started)
            .count());
    return copied;
}

void DurableRuntimeCatalog::request_close_flush() {
    if (flusher_) {
        flusher_->request_flush_all();
    }
}

auto DurableRuntimeCatalog::close() -> Status {
    const std::lock_guard close_lock{close_mutex_};
    const auto cached_status = [&]() -> Status {
        if (!close_error_) {
            return {};
        }
        try {
            return unexpected(*close_error_);
        } catch (const std::bad_alloc&) {
            return unexpected(Error{ErrorCode::resource_exhausted, {}});
        } catch (...) {
            return unexpected(Error{ErrorCode::internal_error, {}});
        }
    };
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return cached_status();
    }

    Status result;
    try {
        if (flusher_) {
            result = flusher_->flush_all_blocking();
            flusher_->stop();
        } else if (!healthy_.load(std::memory_order_acquire)) {
            // Already fail-closed with no periodic flusher: close is a no-op success.
            // Synthesizing unavailable here poisoned Server::join after sticky Writer close.
            result = {};
        } else if (!directory_.healthy()) {
            result = unexpected(Error{ErrorCode::unavailable, {}});
        }
    } catch (const std::bad_alloc&) {
        result = unexpected(Error{ErrorCode::resource_exhausted, {}});
    } catch (...) {
        result = unexpected(Error{ErrorCode::internal_error, {}});
    }
    if (!result) {
        healthy_.store(false, std::memory_order_release);
        close_error_.emplace(std::move(result.error()));
    }
    return cached_status();
}
} // namespace glifistore
