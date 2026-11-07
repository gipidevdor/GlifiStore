#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/integer_math.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/index/swiss_table.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/durable_flush_coordinator.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "persistence/adaptive_batch_sizer.hpp"
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
#include <stdexcept>
#include <string>
#include <utility>

namespace glifistore {
using namespace glifistore::runtime_catalog_detail;

auto DurableRuntimeCatalog::fail_closed(Error error) -> Unexpected {
    healthy_.store(false, std::memory_order_release);
    return unexpected(std::move(error));
}

auto DurableRuntimeCatalog::get(const std::string_view key, const std::uint64_t now_ns)
    -> Result<OwnedValue> {
    return get(std::as_bytes(std::span{key}), now_ns);
}

auto DurableRuntimeCatalog::get(const std::span<const std::byte> key, const std::uint64_t now_ns)
    -> Result<OwnedValue> {
    return get(HashedKey{.key = as_string_view(key), .hash = hash_key_routing(key, worker_routing_)}, now_ns);
}

auto DurableRuntimeCatalog::get(const HashedKey& key, const std::uint64_t now_ns) -> Result<OwnedValue> {
    auto prepared = prepare_get(key, now_ns);
    if (!prepared) {
        return unexpected(prepared.error());
    }
    auto& value = prepared->value;
    if (value) {
        return std::move(value).value();
    }
    auto& cold = prepared->cold;
    if (!cold) {
        return fail(ErrorCode::internal_error, "durable GET preparation produced no result");
    }
    return complete_get(std::move(cold).value());
}

auto DurableRuntimeCatalog::prepare_get(const HashedKey& key, const std::uint64_t now_ns)
    -> Result<PreparedRead> {
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }
    const auto worker_index = route_worker(key.hash, workers_.size());
    auto& worker = *workers_[worker_index];
    auto& metrics = worker.get_path_metrics;
    RecordRef cold_reference;
    std::shared_ptr<const RuntimeSegmentGeneration> cold_pin;
    std::optional<HotRecordSnapshot> hot;
    std::uint64_t mutex_wait_ns = 0;
    std::uint64_t prepare_hold_ns = 0;
    std::uint64_t index_lookup_ns = 0;
    std::uint64_t hot_cache_lookup_ns = 0;
    std::uint64_t generation_pin_lookup_ns = 0;
    std::uint64_t local_hits = 0;
    std::uint64_t local_misses = 0;
    std::uint64_t local_stale = 0;
    ScopeExit publish_prepare_metrics{[&]() noexcept {
        metrics.prepare_calls.fetch_add(1U, std::memory_order_relaxed);
        if (local_hits != 0) {
            metrics.hot_hits.fetch_add(local_hits, std::memory_order_relaxed);
        }
        if (local_misses != 0) {
            metrics.hot_misses.fetch_add(local_misses, std::memory_order_relaxed);
        }
        if (local_stale != 0) {
            metrics.hot_stale_hits.fetch_add(local_stale, std::memory_order_relaxed);
        }
        if constexpr (kGetPathTimingEnabled) {
            atomic_saturating_add(metrics.mutex_wait_ns, mutex_wait_ns);
            atomic_saturating_add(metrics.prepare_hold_ns, prepare_hold_ns);
            atomic_saturating_add(metrics.index_lookup_ns, index_lookup_ns);
            atomic_saturating_add(metrics.hot_cache_lookup_ns, hot_cache_lookup_ns);
            atomic_saturating_add(metrics.generation_pin_lookup_ns, generation_pin_lookup_ns);
        }
    }};
    {
        // Exclusive durable_sync mutate elides worker.mutex — quiesce depth before
        // Index find / deferred TTL so verify_index and catalog GET cannot race
        // insert_or_assign and sticky fail_closed on a torn pin view.
        ExclusiveIndexQuiesce index_quiesce{worker, options_.exclusive_writer && flusher_ == nullptr};
        const auto wait_started = timing_now();
        const std::lock_guard worker_lock{worker.mutex};
        const auto locked_at = timing_now();
        mutex_wait_ns = timing_duration_ns(wait_started, locked_at);
        ScopeExit observe_hold{[&]() noexcept { prepare_hold_ns = timing_elapsed_ns(locked_at); }};
        // Hot path only needs the Worker mutex + atomic health. Catalog shared lock
        // is taken below solely for cold-miss generation pin / manifest identity.
        if (!healthy()) {
            return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
        }

        // Drain deferred TTL only when the backlog is non-empty, and before Index
        // lookup so a reclaimed key becomes an Index miss (not a second cold read).
        // Empty-backlog hot GETs skip this entirely.
        if (!worker.deferred_ttl_reclaims.empty()) {
            if (auto drained =
                    worker.drain_deferred_ttl(std::min<std::size_t>(8, worker.deferred_ttl_reclaims.size()));
                !drained) {
                return fail_closed(drained.error());
            }
        }

        // Ordinary hot path under mutex: Index lookup, hot match + snapshot, unlock.
        // No I/O/CRC/value copy under the lock.
        const auto index_started = timing_now();
        const auto reference = worker.index.find(key);
        index_lookup_ns = timing_elapsed_ns(index_started);
        if (!reference) {
            return fail(ErrorCode::not_found, "key is not present");
        }

        const auto hot_started = timing_now();
        if (const auto* cached = worker.hot_records.find(key); cached != nullptr) {
            if (hot_record_matches(*cached, *reference)) {
                ++local_hits;
                if (cached->expire_at_ns != 0 && now_ns != 0 && cached->expire_at_ns <= now_ns) {
                    if (auto reclaimed = worker.defer_or_reclaim_expired(
                            key, *reference, options_.limits.max_deferred_ttl_reclaims_per_worker);
                        !reclaimed) {
                        hot_cache_lookup_ns = timing_elapsed_ns(hot_started);
                        return fail_closed(reclaimed.error());
                    }
                    hot_cache_lookup_ns = timing_elapsed_ns(hot_started);
                    return fail(ErrorCode::not_found, "key has expired");
                }
                hot.emplace(HotRecordSnapshot::from_entry(*cached));
            } else {
                ++local_stale;
                worker.erase_hot_record(key);
            }
        }
        hot_cache_lookup_ns = timing_elapsed_ns(hot_started);

        if (!hot) {
            ++local_misses;

            const auto pin_started = timing_now();
            const std::shared_lock catalog_lock{catalog_mutex_};
            if (!healthy()) {
                generation_pin_lookup_ns = timing_elapsed_ns(pin_started);
                return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
            }
            const auto catalog_index = catalog_index_for_segment(reference->segment_id);
            if (!catalog_index) {
                generation_pin_lookup_ns = timing_elapsed_ns(pin_started);
                return fail_closed(Error{ErrorCode::corrupted_data,
                                         "durable Index references a Segment absent from the catalog"});
            }
            const auto& found = manifest_.segments[*catalog_index];
            if (found.generation != reference->generation || found.owner_worker != worker.worker_id) {
                generation_pin_lookup_ns = timing_elapsed_ns(pin_started);
                return fail_closed(
                    Error{ErrorCode::corrupted_data,
                          "durable Index reference disagrees with catalog identity or ownership"});
            }
            const auto& pin = generation_pins_[*catalog_index];
            if (!pin || pin->identity.segment_id != reference->segment_id ||
                pin->identity.generation != reference->generation ||
                pin->identity.owner_worker != worker.worker_id) {
                generation_pin_lookup_ns = timing_elapsed_ns(pin_started);
                return fail_closed(Error{ErrorCode::corrupted_data,
                                         "durable Segment generation pin disagrees with the Index"});
            }
            generation_pin_lookup_ns = timing_elapsed_ns(pin_started);
            cold_reference = *reference;
            cold_pin = pin;
        }
    }
    if (hot) {
        return PreparedRead{.value = owned_value_from_hot(*hot)};
    }
    return PreparedRead{.cold = PinnedRead{std::string{key.key}, key.hash, now_ns, worker_index,
                                           cold_reference, std::move(cold_pin)}};
}

auto DurableRuntimeCatalog::snapshot_published_reads(const std::size_t worker_index,
                                                     const bool allow_fail_closed)
    -> Result<PublishedReadSnapshot> try {
    if (worker_index >= workers_.size()) {
        return fail(ErrorCode::invalid_argument,
                    "durable read-generation snapshot targets an invalid Worker");
    }
    if (!allow_fail_closed && !healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }

    auto& worker = *workers_[worker_index];
    // Exclusive durable_sync mutate elides worker.mutex — quiesce depth before
    // Index enumeration so snapshot cannot race insert_or_assign / Index move
    // and sticky fail_closed on a torn pin view (same protocol as prepare_get).
    ExclusiveIndexQuiesce index_quiesce{worker, options_.exclusive_writer && flusher_ == nullptr};
    const std::lock_guard worker_lock{worker.mutex};
    const std::shared_lock catalog_lock{catalog_mutex_};
    if (!allow_fail_closed && !healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }

    auto entries = worker.index.entries();
    PublishedReadSnapshot result;
    result.records.reserve(entries.size());
    for (auto& entry : entries) {
        const auto catalog_index = catalog_index_for_segment(entry.record.segment_id);
        if (!catalog_index) {
            return fail_closed(Error{ErrorCode::corrupted_data,
                                     "durable read-generation snapshot references an absent Segment"});
        }
        const auto& manifest_entry = manifest_.segments[*catalog_index];
        const auto& pin = generation_pins_[*catalog_index];
        if (!pin || manifest_entry.owner_worker != worker.worker_id ||
            manifest_entry.generation != entry.record.generation ||
            pin->identity.segment_id != entry.record.segment_id ||
            pin->identity.generation != entry.record.generation ||
            pin->identity.owner_worker != worker.worker_id) {
            return fail_closed(Error{ErrorCode::corrupted_data,
                                     "durable read-generation snapshot has no exact generation pin"});
        }
        const auto key_hash = hash_key_routing(entry.key, worker_routing_);
        if (route_worker(key_hash, workers_.size()) != worker_index) {
            return fail_closed(
                Error{ErrorCode::corrupted_data, "durable read-generation snapshot contains a foreign key"});
        }
        result.records.push_back(PublishedReadRecord::bind(std::move(entry.key), key_hash, entry.record,
                                                           PublishedReadPin{worker_index, pin}));
    }
    result.catalog_revision = worker.read_catalog_revision.load(std::memory_order_relaxed);
    return result;
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "durable read-generation snapshot allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "durable read-generation snapshot failed");
}

auto DurableRuntimeCatalog::read_catalog_revision(const std::size_t worker_index) const noexcept
    -> std::uint64_t {
    return worker_index < workers_.size()
               ? workers_[worker_index]->read_catalog_revision.load(std::memory_order_acquire)
               : 0U;
}

auto DurableRuntimeCatalog::advance_read_catalog_revision(RuntimeWorker& worker) noexcept -> bool {
    auto revision = worker.read_catalog_revision.load(std::memory_order_relaxed);
    while (revision != std::numeric_limits<std::uint64_t>::max()) {
        if (worker.read_catalog_revision.compare_exchange_weak(
                revision, revision + 1U, std::memory_order_release, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

auto DurableRuntimeCatalog::capture_published_read(const std::size_t worker_index, const HashedKey& key)
    -> Result<PublishedReadRecord> try {
    if (glifistore::fault::consume_fail(glifistore::fault::Site::capture)) {
        return fail(ErrorCode::resource_exhausted, "injected durable read publication failure");
    }
    if (worker_index >= workers_.size() || route_worker(key.hash, workers_.size()) != worker_index) {
        return fail(ErrorCode::invalid_argument, "durable read publication targets the wrong Worker owner");
    }
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }

    auto& worker = *workers_[worker_index];
    // Always take worker.mutex (match snapshot_published_reads) so capture
    // serializes with compaction Phase A/C Index enumerate/swap. Exclusive
    // durable_sync mutate also elides the mutex via hot_path_depth — quiesce
    // depth before Index find so off-Writer / concurrent snapshot paths cannot
    // sticky fail_closed on a torn pin view after a committed PUT.
    ExclusiveIndexQuiesce index_quiesce{worker, options_.exclusive_writer && flusher_ == nullptr};
    const std::lock_guard worker_lock{worker.mutex};
    const std::shared_lock catalog_lock{catalog_mutex_};
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }
    const auto reference = worker.index.find(key);
    if (!reference) {
        return fail(ErrorCode::not_found, "durable read publication key is not present");
    }
    const auto catalog_index = catalog_index_for_segment(reference->segment_id);
    if (!catalog_index) {
        return fail_closed(
            Error{ErrorCode::corrupted_data, "durable read publication references an absent Segment"});
    }
    const auto& manifest_entry = manifest_.segments[*catalog_index];
    const auto& pin = generation_pins_[*catalog_index];
    if (!pin || manifest_entry.owner_worker != worker.worker_id ||
        manifest_entry.generation != reference->generation ||
        pin->identity.segment_id != reference->segment_id ||
        pin->identity.generation != reference->generation || pin->identity.owner_worker != worker.worker_id) {
        return fail_closed(
            Error{ErrorCode::corrupted_data, "durable read publication has no exact generation pin"});
    }
    return PublishedReadRecord::bind(std::string{key.key}, key.hash, *reference,
                                     PublishedReadPin{worker_index, pin});
} catch (const std::bad_alloc&) {

    return fail(ErrorCode::resource_exhausted, "durable read publication allocation failed");
} catch (...) {
    return fail(ErrorCode::internal_error, "durable read publication capture failed");
}

auto DurableRuntimeCatalog::prepare_published_get(PublishedReadRecord read, const std::uint64_t now_ns)
    -> Result<PreparedRead> {
    // Immutable pin-backed reads must remain servable after sticky fail-closed:
    // sibling drain-snapshot success-ACKs keys into the published generation; RAW
    // after ACK must not die on healthy(). Mutable Index prepare_get still gates.
    const auto worker_index = read.pin_.worker_index_;
    if (worker_index >= workers_.size() || route_worker(read.key_hash_, workers_.size()) != worker_index ||
        !read.pin_.matches(read.reference_) ||
        read.pin_.generation_->identity.owner_worker != workers_[worker_index]->worker_id) {
        return fail_closed(Error{ErrorCode::corrupted_data,
                                 "immutable durable read publication is internally inconsistent"});
    }
    return PreparedRead{.cold = PinnedRead{std::move(read.key_), read.key_hash_, now_ns, worker_index,
                                           read.reference_, std::move(read.pin_.generation_), true}};
}

auto DurableRuntimeCatalog::prepare_published_get(const PublishedReadView read, const std::uint64_t now_ns)
    -> Result<PreparedRead> {
    // Same RAW contract as the owned overload: published pins outlive fail-closed.
    if (read.pin_ == nullptr || !read.pin_->generation_) {
        return fail_closed(Error{ErrorCode::corrupted_data, "borrowed durable read has no generation pin"});
    }
    const auto worker_index = read.pin_->worker_index_;
    if (worker_index >= workers_.size() || route_worker(read.key_hash_, workers_.size()) != worker_index ||
        !read.pin_->matches(read.reference_) ||
        read.pin_->generation_->identity.owner_worker != workers_[worker_index]->worker_id) {
        return fail_closed(
            Error{ErrorCode::corrupted_data, "borrowed durable read publication is inconsistent"});
    }
    return PreparedRead{.borrowed_cold = BorrowedPinnedRead{read.key_, read.key_hash_, now_ns, worker_index,
                                                            read.reference_, read.pin_->generation_.get()}};
}

auto DurableRuntimeCatalog::complete_generation_get(
    const std::string_view key, const std::uint64_t key_hash, const std::uint64_t now_ns,
    const std::size_t worker_index, const RecordRef& reference, const RuntimeSegmentGeneration& generation,
    const detail::ColdReadCancellation* cancellation, std::vector<std::byte>* scratch) -> Result<OwnedValue> {
    if (cancellation != nullptr && cancellation->cancelled()) {
        return fail(ErrorCode::unavailable, "durable cold read was cancelled");
    }
    auto& metrics = workers_[worker_index]->get_path_metrics;
    metrics.complete_calls.fetch_add(1U, std::memory_order_relaxed);
    std::vector<std::byte> local_scratch;
    auto& read_scratch = scratch == nullptr ? local_scratch : *scratch;
    const auto key_bytes =
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(key.data()), key.size()};
    ReadContext context{.expected_key = key_bytes, .expected_hash = key_hash, .now_ns = now_ns};
    const auto cold_started = timing_now();
    auto visited =
        generation.file.visit_runtime_record(reference, read_scratch, &context, &copy_verified_value);
    if constexpr (kGetPathTimingEnabled) {
        atomic_saturating_add(metrics.cold_read_ns, timing_elapsed_ns(cold_started));
        atomic_saturating_add(metrics.crc_value_copy_ns, context.crc_value_copy_ns);
    }
    if (!visited) {
        if (visited.error().code == ErrorCode::not_found) {
            return unexpected(visited.error());
        }
        return fail_closed(visited.error());
    }
    return std::move(context.value);
}

auto DurableRuntimeCatalog::complete_get(BorrowedPinnedRead read,
                                         const detail::ColdReadCancellation* cancellation,
                                         std::vector<std::byte>* scratch) -> Result<OwnedValue> {
    // Pin identity only — mirror generation_linearized_ owned complete (no healthy gate).
    if (read.worker_index_ >= workers_.size() || read.generation_ == nullptr ||
        route_worker(read.key_hash_, workers_.size()) != read.worker_index_ ||
        read.generation_->identity.owner_worker != workers_[read.worker_index_]->worker_id ||
        read.generation_->identity.segment_id != read.reference_.segment_id ||
        read.generation_->identity.generation != read.reference_.generation) {
        return fail_closed(
            Error{ErrorCode::corrupted_data, "borrowed cold read lost its generation identity"});
    }
    return complete_generation_get(read.key_, read.key_hash_, read.now_ns_, read.worker_index_,
                                   read.reference_, *read.generation_, cancellation, scratch);
}

auto DurableRuntimeCatalog::complete_get(PinnedRead read, const detail::ColdReadCancellation* cancellation,
                                         std::vector<std::byte>* scratch) -> Result<OwnedValue> {
    constexpr std::size_t kMaximumRelinearizationAttempts = 8;
    if (read.generation_linearized_) {
        return complete_generation_get(read.key_, read.key_hash_, read.now_ns_, read.worker_index_,
                                       read.reference_, *read.generation_, cancellation, scratch);
    }
    std::vector<std::byte> local_scratch;
    auto& read_scratch = scratch == nullptr ? local_scratch : *scratch;
    for (std::size_t attempt = 0; attempt < kMaximumRelinearizationAttempts; ++attempt) {
        if (cancellation != nullptr && cancellation->cancelled()) {
            return fail(ErrorCode::unavailable, "durable cold read was cancelled");
        }
        auto& worker = *workers_[read.worker_index_];
        auto& metrics = worker.get_path_metrics;
        metrics.complete_calls.fetch_add(1U, std::memory_order_relaxed);
        const auto key_bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(read.key_.data()), read.key_.size()};
        ReadContext context{
            .expected_key = key_bytes, .expected_hash = read.key_hash_, .now_ns = read.now_ns_};
        const auto cold_started = timing_now();
        auto visited = read.generation_->file.visit_runtime_record(read.reference_, read_scratch, &context,
                                                                   &copy_verified_value);
        if constexpr (kGetPathTimingEnabled) {
            atomic_saturating_add(metrics.cold_read_ns, timing_elapsed_ns(cold_started));
            atomic_saturating_add(metrics.crc_value_copy_ns, context.crc_value_copy_ns);
        }

        // Linearization point for a cold GET: the Worker index must still
        // designate both the exact RecordRef and immutable generation pin
        // captured by prepare_get(). No file I/O or CRC work holds either lock.
        bool still_current{};
        {
            ExclusiveIndexQuiesce index_quiesce{worker, options_.exclusive_writer && flusher_ == nullptr};
            const auto wait_started = timing_now();
            const std::lock_guard worker_lock{worker.mutex};
            const auto locked_at = timing_now();
            if constexpr (kGetPathTimingEnabled) {
                atomic_saturating_add(metrics.mutex_wait_ns, timing_duration_ns(wait_started, locked_at));
            }
            ScopeExit observe_hold{[&]() noexcept {
                if constexpr (kGetPathTimingEnabled) {
                    atomic_saturating_add(metrics.complete_revalidate_hold_ns, timing_elapsed_ns(locked_at));
                }
            }};
            if (!healthy()) {
                return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
            }
            const HashedKey key{.key = read.key_, .hash = read.key_hash_};
            const auto index_started = timing_now();
            const auto current = worker.index.find(key);
            if constexpr (kGetPathTimingEnabled) {
                atomic_saturating_add(metrics.index_lookup_ns, timing_elapsed_ns(index_started));
            }
            // Catalog lock only when the Index still names the captured RecordRef;
            // churned keys skip pin identity work and catalog shared-lock contention.
            if (current && *current == read.reference_) {
                const auto pin_started = timing_now();
                const std::shared_lock catalog_lock{catalog_mutex_};
                if (!healthy()) {
                    return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
                }
                const auto catalog_index = catalog_index_for_segment(current->segment_id);
                still_current =
                    catalog_index.has_value() && generation_pins_[*catalog_index] == read.generation_;
                if constexpr (kGetPathTimingEnabled) {
                    atomic_saturating_add(metrics.generation_pin_lookup_ns, timing_elapsed_ns(pin_started));
                }
            }
        }
        if (!still_current) {
            if (attempt + 1U == kMaximumRelinearizationAttempts) {
                return fail(ErrorCode::sequence_conflict,
                            "durable cold read exceeded its relinearization retry budget");
            }
            atomic_saturating_add(metrics.relinearization_retries, 1U);
            const HashedKey key{.key = read.key_, .hash = read.key_hash_};
            auto prepared = prepare_get(key, read.now_ns_);
            if (!prepared) {
                return unexpected(prepared.error());
            }
            auto& value = prepared->value;
            if (value) {
                return std::move(value).value();
            }
            auto& cold = prepared->cold;
            if (!cold) {
                return fail(ErrorCode::internal_error, "durable GET retry produced no result");
            }
            read = std::move(cold).value();
            continue;
        }
        if (!visited) {
            if (visited.error().code == ErrorCode::not_found) {
                // Visitor not_found is validated expiry only. Defer Index reclaim
                // while verifying the exact RecordRef when the backlog drains.
                {
                    ExclusiveIndexQuiesce index_quiesce{worker,
                                                        options_.exclusive_writer && flusher_ == nullptr};
                    const auto wait_started = timing_now();
                    const std::lock_guard worker_lock{worker.mutex};
                    const auto locked_at = timing_now();
                    if constexpr (kGetPathTimingEnabled) {
                        atomic_saturating_add(metrics.mutex_wait_ns,
                                              timing_duration_ns(wait_started, locked_at));
                    }
                    ScopeExit observe_hold{[&]() noexcept {
                        if constexpr (kGetPathTimingEnabled) {
                            atomic_saturating_add(metrics.complete_revalidate_hold_ns,
                                                  timing_elapsed_ns(locked_at));
                        }
                    }};
                    // Index + deferred TTL reclaim are Worker-local; no catalog pin work.
                    if (healthy()) {
                        const HashedKey key{.key = read.key_, .hash = read.key_hash_};
                        const auto current = worker.index.find(key);
                        if (current && *current == read.reference_) {
                            if (auto reclaimed = worker.defer_or_reclaim_expired(
                                    key, read.reference_,
                                    options_.limits.max_deferred_ttl_reclaims_per_worker);
                                !reclaimed) {
                                return fail_closed(reclaimed.error());
                            }
                        }
                    }
                }
                return unexpected(visited.error());
            }
            return fail_closed(visited.error());
        }
        return std::move(context.value);
    }
    return fail(ErrorCode::internal_error, "durable cold read retry loop escaped its bound");
}

} // namespace glifistore
