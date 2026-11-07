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
#include <string>
#include <utility>

namespace glifistore {
using namespace glifistore::runtime_catalog_detail;

auto DurableRuntimeCatalog::healthy() const noexcept -> bool {
    return !closed_.load(std::memory_order_acquire) && healthy_.load(std::memory_order_acquire) &&
           directory_.healthy();
}

void DurableRuntimeCatalog::mark_fail_closed() noexcept {
    abandon_pending_batches();
}

auto DurableRuntimeCatalog::worker_count() const noexcept -> std::size_t {
    return workers_.size();
}

auto DurableRuntimeCatalog::manifest() const -> Manifest {
    const std::shared_lock lock{catalog_mutex_};
    return manifest_;
}

auto DurableRuntimeCatalog::namespace_audit() const -> NamespaceAuditReport {
    const std::shared_lock lock{catalog_mutex_};
    return namespace_audit_;
}

auto DurableRuntimeCatalog::recovery_stats() const noexcept -> const DurableRecoveryStats& {
    return recovery_stats_;
}

auto DurableRuntimeCatalog::hot_cache_stats() const -> std::vector<DurableHotCacheWorkerStats> {
    std::vector<DurableHotCacheWorkerStats> result;
    result.reserve(workers_.size());
    for (std::size_t index = 0; index < workers_.size(); ++index) {
        auto& worker = *workers_[index];
        const auto& metrics = worker.get_path_metrics;
        std::size_t resident_entries{};
        std::uint64_t resident_bytes{};
        std::size_t staged_entries{};
        std::uint64_t staged_bytes{};
        std::uint64_t table_bytes{};
        std::uint64_t total_accounted{};
        {
            const std::lock_guard lock{worker.mutex};
            resident_entries = worker.hot_records.size();
            resident_bytes = worker.hot_record_resident_bytes;
            staged_entries = worker.hot_record_staged_entries;
            staged_bytes = worker.hot_record_staged_bytes;
            table_bytes = hot_cache_table_bytes(worker.hot_records.capacity());
            total_accounted = worker.hot_cache_total_bytes();
        }
        const auto hits = metrics.hot_hits.load(std::memory_order_relaxed);
        const auto misses = metrics.hot_misses.load(std::memory_order_relaxed);
        result.push_back({
            .worker_id = worker.worker_id,
            .resident_entries = resident_entries,
            .resident_bytes = resident_bytes,
            .staged_entries = staged_entries,
            .staged_bytes = staged_bytes,
            .bucket_bytes = table_bytes,
            .total_accounted_bytes = total_accounted,
            .byte_budget = hot_cache_worker_budget(index, workers_.size(), options_.limits),
            .staging_byte_budget = options_.limits.max_hot_cache_staging_bytes_per_worker,
            .entry_budget = options_.limits.max_hot_cache_entries_per_worker,
            .hits = hits,
            .misses = misses,
            .stale_hits = metrics.hot_stale_hits.load(std::memory_order_relaxed),
            .evictions = metrics.hot_evictions.load(std::memory_order_relaxed),
            .admission_bypasses = metrics.admission_bypasses.load(std::memory_order_relaxed),
            .size_rejected = metrics.size_rejected.load(std::memory_order_relaxed),
            .expired_gets = metrics.expired_ttl_gets.load(std::memory_order_relaxed),
            .hit_rate_bp = (hits + misses) == 0 ? 0 : (hits * 10'000ULL) / (hits + misses),
            .enabled = options_.limits.hot_cache_enabled &&
                       hot_cache_worker_budget(index, workers_.size(), options_.limits) > 0 &&
                       options_.limits.max_hot_cache_entries_per_worker > 0 &&
                       options_.limits.max_hot_cache_value_bytes > 0,
            .max_value_bytes = options_.limits.max_hot_cache_value_bytes,
        });
    }
    return result;
}

auto DurableRuntimeCatalog::get_path_stats() const -> std::vector<DurableGetPathWorkerStats> {
    std::vector<DurableGetPathWorkerStats> result;
    result.reserve(workers_.size());
    for (const auto& worker : workers_) {
        const auto& metrics = worker->get_path_metrics;
        std::size_t resident_entries{};
        std::uint64_t resident_bytes{};
        {
            const std::lock_guard lock{worker->mutex};
            resident_entries = worker->hot_records.size();
            resident_bytes = worker->hot_record_resident_bytes;
        }
        result.push_back({
            .worker_id = worker->worker_id,
            .prepare_calls = metrics.prepare_calls.load(std::memory_order_relaxed),
            .complete_calls = metrics.complete_calls.load(std::memory_order_relaxed),
            .mutex_wait_ns = metrics.mutex_wait_ns.load(std::memory_order_relaxed),
            .prepare_hold_ns = metrics.prepare_hold_ns.load(std::memory_order_relaxed),
            .complete_revalidate_hold_ns =
                metrics.complete_revalidate_hold_ns.load(std::memory_order_relaxed),
            .index_lookup_ns = metrics.index_lookup_ns.load(std::memory_order_relaxed),
            .hot_cache_lookup_ns = metrics.hot_cache_lookup_ns.load(std::memory_order_relaxed),
            .generation_pin_lookup_ns = metrics.generation_pin_lookup_ns.load(std::memory_order_relaxed),
            .cold_read_ns = metrics.cold_read_ns.load(std::memory_order_relaxed),
            .crc_value_copy_ns = metrics.crc_value_copy_ns.load(std::memory_order_relaxed),
            .relinearization_retries = metrics.relinearization_retries.load(std::memory_order_relaxed),
            .hot_hits = metrics.hot_hits.load(std::memory_order_relaxed),
            .hot_misses = metrics.hot_misses.load(std::memory_order_relaxed),
            .hot_stale = metrics.hot_stale_hits.load(std::memory_order_relaxed),
            .hot_evictions = metrics.hot_evictions.load(std::memory_order_relaxed),
            .expired_ttl_gets = metrics.expired_ttl_gets.load(std::memory_order_relaxed),
            .hot_resident_entries = resident_entries,
            .hot_resident_bytes = resident_bytes,
        });
    }
    return result;
}

auto DurableRuntimeCatalog::batch_stats() const -> std::vector<DurableBatchWorkerStats> {
    std::vector<DurableBatchWorkerStats> result;
    result.reserve(workers_.size());
    for (const auto& worker : workers_) {
        const auto& metrics = worker->batch_metrics;
        result.push_back({
            .worker_id = worker->worker_id,
            .enabled = options_.batch.has_value(),
            .pending_records = metrics.pending_records.load(std::memory_order_relaxed),
            .pending_bytes = metrics.pending_bytes.load(std::memory_order_relaxed),
            .current_record_target = metrics.current_record_target.load(std::memory_order_relaxed),
            .flush_attempts = metrics.flush_attempts.load(std::memory_order_relaxed),
            .committed_batches = metrics.committed_batches.load(std::memory_order_relaxed),
            .failed_batches = metrics.failed_batches.load(std::memory_order_relaxed),
            .committed_records = metrics.committed_records.load(std::memory_order_relaxed),
            .committed_bytes = metrics.committed_bytes.load(std::memory_order_relaxed),
            .maximum_batch_records = metrics.maximum_batch_records.load(std::memory_order_relaxed),
            .maximum_batch_bytes = metrics.maximum_batch_bytes.load(std::memory_order_relaxed),
            .total_commit_duration_ns = metrics.total_commit_duration_ns.load(std::memory_order_relaxed),
            .maximum_commit_duration_ns = metrics.maximum_commit_duration_ns.load(std::memory_order_relaxed),
            .record_limit_closes = metrics.record_limit_closes.load(std::memory_order_relaxed),
            .byte_limit_closes = metrics.byte_limit_closes.load(std::memory_order_relaxed),
            .adaptive_target_closes = metrics.adaptive_target_closes.load(std::memory_order_relaxed),
            .deadline_closes = metrics.deadline_closes.load(std::memory_order_relaxed),
        });
    }
    return result;
}

void DurableRuntimeCatalog::record_rotation_final_commit(const std::uint64_t duration_ns,
                                                         const bool committed) noexcept {
    const auto previous_version = begin_atomic_stats_publication(rotation_stats_version_);
    rotation_final_record_commit_attempts_.fetch_add(1U, std::memory_order_relaxed);
    if (committed) {
        rotation_final_record_commits_.fetch_add(1U, std::memory_order_relaxed);
    }
    last_rotation_final_record_commit_ns_.store(duration_ns, std::memory_order_relaxed);
    atomic_saturating_add(total_rotation_final_record_commit_ns_, duration_ns);
    atomic_observe_max(maximum_rotation_final_record_commit_ns_, duration_ns);
    end_atomic_stats_publication(rotation_stats_version_, previous_version);
}

auto DurableRuntimeCatalog::rotation_stats() const noexcept -> DurableRotationStats {
    while (true) {
        const auto before = rotation_stats_version_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        const DurableRotationStats result{
            .attempts = rotation_attempts_.load(std::memory_order_relaxed),
            .committed = rotations_committed_.load(std::memory_order_relaxed),
            .compaction_waits = rotation_compaction_waits_.load(std::memory_order_relaxed),
            .final_record_commit_attempts =
                rotation_final_record_commit_attempts_.load(std::memory_order_relaxed),
            .final_record_commits = rotation_final_record_commits_.load(std::memory_order_relaxed),
            .last_publication_wait_duration_ns =
                last_rotation_publication_wait_ns_.load(std::memory_order_relaxed),
            .total_publication_wait_duration_ns =
                total_rotation_publication_wait_ns_.load(std::memory_order_relaxed),
            .maximum_publication_wait_duration_ns =
                maximum_rotation_publication_wait_ns_.load(std::memory_order_relaxed),
            .last_seal_duration_ns = last_rotation_seal_ns_.load(std::memory_order_relaxed),
            .total_seal_duration_ns = total_rotation_seal_ns_.load(std::memory_order_relaxed),
            .maximum_seal_duration_ns = maximum_rotation_seal_ns_.load(std::memory_order_relaxed),
            .last_create_duration_ns = last_rotation_create_ns_.load(std::memory_order_relaxed),
            .total_create_duration_ns = total_rotation_create_ns_.load(std::memory_order_relaxed),
            .maximum_create_duration_ns = maximum_rotation_create_ns_.load(std::memory_order_relaxed),
            .last_manifest_publication_duration_ns =
                last_rotation_manifest_publication_ns_.load(std::memory_order_relaxed),
            .total_manifest_publication_duration_ns =
                total_rotation_manifest_publication_ns_.load(std::memory_order_relaxed),
            .maximum_manifest_publication_duration_ns =
                maximum_rotation_manifest_publication_ns_.load(std::memory_order_relaxed),
            .last_execution_duration_ns = last_rotation_execution_ns_.load(std::memory_order_relaxed),
            .total_execution_duration_ns = total_rotation_execution_ns_.load(std::memory_order_relaxed),
            .maximum_execution_duration_ns = maximum_rotation_execution_ns_.load(std::memory_order_relaxed),
            .last_total_duration_ns = last_rotation_total_ns_.load(std::memory_order_relaxed),
            .total_duration_ns = total_rotation_ns_.load(std::memory_order_relaxed),
            .maximum_total_duration_ns = maximum_rotation_total_ns_.load(std::memory_order_relaxed),
            .last_final_record_commit_duration_ns =
                last_rotation_final_record_commit_ns_.load(std::memory_order_relaxed),
            .total_final_record_commit_duration_ns =
                total_rotation_final_record_commit_ns_.load(std::memory_order_relaxed),
            .maximum_final_record_commit_duration_ns =
                maximum_rotation_final_record_commit_ns_.load(std::memory_order_relaxed),
        };
        if (before == rotation_stats_version_.load(std::memory_order_acquire)) {
            return result;
        }
    }
}

auto DurableRuntimeCatalog::next_sequence(const std::size_t worker_index) const -> Result<SequenceNumber> {
    if (worker_index >= workers_.size()) {
        return fail(ErrorCode::invalid_argument, "Worker index is outside the durable runtime");
    }
    const std::lock_guard lock{workers_[worker_index]->mutex};
    return workers_[worker_index]->next_sequence;
}

auto DurableRuntimeCatalog::active_segment(const std::size_t worker_index) const -> Result<SegmentId> {
    if (worker_index >= workers_.size()) {
        return fail(ErrorCode::invalid_argument, "Worker index is outside the durable runtime");
    }
    const std::lock_guard lock{workers_[worker_index]->mutex};
    return workers_[worker_index]->active_segment;
}

auto DurableRuntimeCatalog::next_compaction_worker(const std::size_t start_worker) const
    -> Result<std::optional<std::size_t>> {
    if (start_worker >= workers_.size()) {
        return fail(ErrorCode::invalid_argument, "compaction cursor is outside the durable runtime");
    }
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }
    const std::shared_lock catalog_lock{catalog_mutex_};
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }

    std::optional<std::size_t> candidate;
    auto best_distance = workers_.size();
    for (const auto& entry : manifest_.segments) {
        if (entry.role != ManifestSegmentRole::sealed) {
            continue;
        }
        const auto owner = static_cast<std::size_t>(entry.owner_worker.value);
        if (owner >= workers_.size()) {
            return fail(ErrorCode::corrupted_data, "sealed Segment owner is outside the durable runtime");
        }
        const auto distance =
            owner >= start_worker ? owner - start_worker : workers_.size() - start_worker + owner;
        if (distance < best_distance) {
            candidate = owner;
            best_distance = distance;
            if (distance == 0) {
                break;
            }
        }
    }
    return candidate;
}

auto DurableRuntimeCatalog::maintenance_observation(const std::size_t start_worker,
                                                    const std::uint64_t now_ns,
                                                    const bool probe_unread_expired_ttl)
    -> Result<MaintenanceObservation> {
    if (start_worker >= workers_.size()) {
        return fail(ErrorCode::invalid_argument,
                    "maintenance compaction cursor is outside the durable runtime");
    }
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }
    std::shared_lock catalog_lock{catalog_mutex_};
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }

    MaintenanceObservation observation{
        .durable = true,
        .segment_count = manifest_.segments.size(),
        .max_segment_count = options_.limits.max_segment_count,
        .reserved_free_bytes = options_.limits.reserved_free_bytes,
    };
    if (segments_.size() != manifest_.segments.size()) {
        return fail(ErrorCode::corrupted_data, "maintenance observation catalog metadata is not aligned");
    }
    auto best_distance = workers_.size();
    for (std::size_t index = 0; index < manifest_.segments.size(); ++index) {
        const auto& entry = manifest_.segments[index];
        if (entry.role == ManifestSegmentRole::sealed) {
            ++observation.sealed_segment_count;
            const auto owner = static_cast<std::size_t>(entry.owner_worker.value);
            if (owner >= workers_.size()) {
                return fail(ErrorCode::corrupted_data, "sealed Segment owner is outside the durable runtime");
            }
            const auto distance =
                owner >= start_worker ? owner - start_worker : workers_.size() - start_worker + owner;
            if (distance < best_distance) {
                observation.compaction_candidate_worker = owner;
                best_distance = distance;
            }
        }
    }
    if (observation.compaction_candidate_worker) {
        const auto candidate = *observation.compaction_candidate_worker;
        for (std::size_t index = 0; index < manifest_.segments.size(); ++index) {
            const auto& entry = manifest_.segments[index];
            if (entry.role != ManifestSegmentRole::sealed || entry.owner_worker.value != candidate) {
                continue;
            }
            const auto committed_end = segments_[index].selected.commit.committed_end;
            if (committed_end < kSegmentHeaderReservedBytes || committed_end > kSegmentSizeBytes) {
                return fail(ErrorCode::corrupted_data,
                            "sealed Segment committed extent is outside v1 bounds");
            }
            const auto record_bytes = static_cast<std::uint64_t>(committed_end - kSegmentHeaderReservedBytes);
            if (record_bytes >
                std::numeric_limits<std::uint64_t>::max() - observation.candidate_sealed_record_bytes) {
                return fail(ErrorCode::arithmetic_overflow,
                            "maintenance sealed Record byte count overflows uint64_t");
            }
            observation.candidate_sealed_record_bytes += record_bytes;
        }
        observation.candidate_live_record_bytes =
            workers_[candidate]->sealed_live_record_bytes.load(std::memory_order_acquire);
        if (observation.candidate_live_record_bytes > observation.candidate_sealed_record_bytes) {
            return fail(ErrorCode::corrupted_data,
                        "maintenance live Record bytes exceed sealed committed bytes");
        }
        observation.candidate_dead_record_bytes =
            observation.candidate_sealed_record_bytes - observation.candidate_live_record_bytes;
        observation.candidate_dead_byte_ratio_bp =
            observation.candidate_sealed_record_bytes == 0
                ? 10'000U
                : basis_points(observation.candidate_dead_record_bytes,
                               observation.candidate_sealed_record_bytes);
    }
    // Match rotate_active: require_durable_available_space(kSegmentSizeBytes + next_manifest_bytes).
    const auto next_count = observation.segment_count + 1U;
    if (auto next_manifest = durable_manifest_bytes(next_count); next_manifest) {
        if (*next_manifest <= std::numeric_limits<std::uint64_t>::max() - kSegmentSizeBytes) {
            observation.rotate_additional_bytes = kSegmentSizeBytes + *next_manifest;
        } else {
            observation.rotate_additional_bytes = std::numeric_limits<std::uint64_t>::max();
        }
    } else {
        observation.rotate_additional_bytes = std::numeric_limits<std::uint64_t>::max();
    }
    if (auto free = directory_.available_space_bytes(); free) {
        observation.available_free_bytes = *free;
    } else if (free.error().code != ErrorCode::unavailable && free.error().code != ErrorCode::io_error) {
        return unexpected(free.error());
    }

    if (probe_unread_expired_ttl && now_ns != 0 && observation.compaction_candidate_worker) {
        struct ProbeContext {
            std::uint64_t now_ns{};
            std::uint64_t key_hash{};
            bool expired{};
        };

        const RecordVisitor probe_visitor = [](void* opaque, const RecordView& record) -> Status {
            auto& context = *static_cast<ProbeContext*>(opaque);
            if (record.opcode != Opcode::put || record.key_hash != context.key_hash) {
                return fail(ErrorCode::corrupted_data,
                            "unread TTL probe Index entry disagrees with its source Record");
            }
            context.expired = record.expired(context.now_ns);
            return {};
        };

        const auto candidate = *observation.compaction_candidate_worker;
        auto& worker = *workers_[candidate];
        // Drop the observation catalog lock before any exclusive-Writer depth wait
        // (holding catalog across hot_path_depth wait deadlocks mutex-elided mutate).
        // Also avoids a same-thread nested shared lock on catalog_mutex_.
        catalog_lock.unlock();

        // Exclusive durable_sync elides worker.mutex on mutate — same Index ownership
        // protocol as compaction Phase A / prepare_get.
        const bool elide_worker_mutex = options_.exclusive_writer && flusher_ == nullptr;
        ExclusiveIndexQuiesce probe_gate{worker, elide_worker_mutex};

        const std::lock_guard worker_lock{worker.mutex};
        const std::shared_lock probe_catalog_lock{catalog_mutex_};
        if (!healthy()) {
            return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
        }
        if (generation_pins_.size() != manifest_.segments.size()) {
            return fail(ErrorCode::corrupted_data, "unread TTL probe catalog metadata is not aligned");
        }

        for (const auto& entry : worker.index.entries()) {
            if (entry.record.segment_id == worker.active_segment) {
                continue;
            }
            const auto found = std::lower_bound(manifest_.segments.begin(), manifest_.segments.end(),
                                                entry.record.segment_id,
                                                [](const ManifestSegmentEntry& segment, const SegmentId id) {
                                                    return segment.segment_id.value < id.value;
                                                });
            if (found == manifest_.segments.end() || found->segment_id != entry.record.segment_id ||
                found->role != ManifestSegmentRole::sealed || found->owner_worker.value != candidate) {
                continue;
            }
            const auto catalog_index = static_cast<std::size_t>(found - manifest_.segments.begin());
            const auto& pin = generation_pins_[catalog_index];
            if (!pin || pin->identity.segment_id != entry.record.segment_id ||
                pin->identity.generation != entry.record.generation ||
                pin->identity.owner_worker != worker.worker_id) {
                return fail(ErrorCode::corrupted_data,
                            "unread TTL probe generation pin disagrees with the Index");
            }
            ProbeContext context{.now_ns = now_ns, .key_hash = hash_key_routing(entry.key, worker_routing_)};
            if (auto visited = pin->file.visit_runtime_record(entry.record, &context, probe_visitor);
                !visited) {
                return unexpected(visited.error());
            }
            if (!context.expired) {
                continue;
            }
            ++observation.candidate_unread_expired_sealed_record_count;
            if (entry.record.size.value > std::numeric_limits<std::uint64_t>::max() -
                                              observation.candidate_unread_expired_sealed_record_bytes) {
                return fail(ErrorCode::arithmetic_overflow,
                            "unread TTL probe sealed Record byte count overflows uint64_t");
            }
            observation.candidate_unread_expired_sealed_record_bytes += entry.record.size.value;
        }
        observation.unread_ttl_probe_performed = true;
        probe_gate.clear();
    }
    return observation;
}

auto DurableRuntimeCatalog::snapshot_live_keys() -> Result<std::vector<std::string>> {
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }
    std::vector<std::string> keys;
    const bool elide_worker_mutex = options_.exclusive_writer && flusher_ == nullptr;
    for (std::size_t worker_index = 0; worker_index < workers_.size(); ++worker_index) {
        auto& worker = *workers_[worker_index];
        ExclusiveIndexQuiesce index_quiesce{worker, elide_worker_mutex};
        const std::lock_guard lock{worker.mutex};
        const std::shared_lock catalog_lock{catalog_mutex_};
        if (!healthy()) {
            return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
        }
        auto entries = worker.index.entries();
        for (auto& entry : entries) {
            if (route_worker(hash_key_routing(entry.key, worker_routing_), workers_.size()) != worker_index) {
                return fail(ErrorCode::corrupted_data, "durable Index entry is routed to the wrong Worker");
            }
            keys.push_back(std::move(entry.key));
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

auto DurableRuntimeCatalog::verify_index() -> Status {
    if (!healthy()) {
        return fail(ErrorCode::unavailable, "durable runtime is fail-closed");
    }
    auto keys = snapshot_live_keys();
    if (!keys) {
        return unexpected(keys.error());
    }
    for (const auto& key : *keys) {
        if (auto verified = get(key); !verified) {
            return unexpected(verified.error());
        }
    }
    return {};
}

} // namespace glifistore
