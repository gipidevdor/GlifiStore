#include "persistence/runtime_catalog_detail.hpp"

#include "glifistore/core/integer_math.hpp"
#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/namespace_audit.hpp"
#include "glifistore/persistence/recovery.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/record.hpp"
#include "persistence/hot_record_table.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace glifistore::runtime_catalog_detail {

[[nodiscard]] auto as_string_view(const std::span<const std::byte> bytes) noexcept -> std::string_view {
    if (bytes.empty()) {
        return {};
    }
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] auto steady_duration_ns(const std::chrono::steady_clock::time_point start,
                                      const std::chrono::steady_clock::time_point end) noexcept
    -> std::uint64_t {
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return count <= 0 ? 0U : static_cast<std::uint64_t>(count);
}

[[nodiscard]] auto steady_elapsed_ns(const std::chrono::steady_clock::time_point start) noexcept
    -> std::uint64_t {
    return steady_duration_ns(start, std::chrono::steady_clock::now());
}

[[nodiscard]] auto timing_now() noexcept -> std::optional<std::chrono::steady_clock::time_point> {
    if constexpr (!kGetPathTimingEnabled) {
        return std::nullopt;
    }
    return std::chrono::steady_clock::now();
}

[[nodiscard]] auto
timing_elapsed_ns(const std::optional<std::chrono::steady_clock::time_point> start) noexcept
    -> std::uint64_t {
    if constexpr (!kGetPathTimingEnabled) {
        return 0;
    }
    if (!start) {
        return 0;
    }
    return steady_elapsed_ns(*start);
}

[[nodiscard]] auto timing_duration_ns(const std::optional<std::chrono::steady_clock::time_point> start,
                                      const std::optional<std::chrono::steady_clock::time_point> end) noexcept
    -> std::uint64_t {
    if constexpr (!kGetPathTimingEnabled) {
        return 0;
    }
    if (!start || !end) {
        return 0;
    }
    return steady_duration_ns(*start, *end);
}

auto copy_verified_value(void* opaque, const RecordView& record) -> Status {
    auto& context = *static_cast<ReadContext*>(opaque);
    const auto copy_started = timing_now();
    if (record.opcode != Opcode::put) {
        context.crc_value_copy_ns = timing_elapsed_ns(copy_started);
        return fail(ErrorCode::corrupted_data, "durable Index references a non-value Record");
    }
    if (record.key_hash != context.expected_hash || !std::ranges::equal(record.key, context.expected_key)) {
        context.crc_value_copy_ns = timing_elapsed_ns(copy_started);
        return fail(ErrorCode::corrupted_data, "durable Index key does not match its referenced Record");
    }
    if (record.expired(context.now_ns)) {
        context.crc_value_copy_ns = timing_elapsed_ns(copy_started);
        return fail(ErrorCode::not_found, "key has expired");
    }
    context.value =
        OwnedValue::from_bytes(record.value, record.sequence.value, record.expire_at_ns, record.type);
    context.crc_value_copy_ns = timing_elapsed_ns(copy_started);
    return {};
}

auto mutation_failure(const DurableMutationOutcome outcome, Error error) -> DurableMutationResult {
    return {.outcome = outcome, .sequence = std::nullopt, .error = std::move(error)};
}

void atomic_saturating_add(std::atomic_uint64_t& destination, const std::uint64_t value) noexcept {
    auto current = destination.load(std::memory_order_relaxed);
    while (true) {
        const auto next = value > std::numeric_limits<std::uint64_t>::max() - current
                              ? std::numeric_limits<std::uint64_t>::max()
                              : current + value;
        if (destination.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
            return;
        }
    }
}

[[nodiscard]] auto begin_atomic_stats_publication(std::atomic_uint64_t& version) noexcept -> std::uint64_t {
    auto current = version.load(std::memory_order_relaxed);
    while (true) {
        if ((current & 1U) != 0) {
            current = version.load(std::memory_order_acquire);
            continue;
        }
        if (version.compare_exchange_weak(current, current + 1U, std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
            return current;
        }
    }
}

void end_atomic_stats_publication(std::atomic_uint64_t& version, const std::uint64_t previous) noexcept {
    version.store(previous + 2U, std::memory_order_release);
}

[[nodiscard]] auto hot_cache_table_bytes(const std::size_t capacity) noexcept -> std::uint64_t {
    const auto per_slot = detail::hot_record_slot_bytes();
    if (capacity > std::numeric_limits<std::uint64_t>::max() / per_slot) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(capacity) * per_slot;
}

[[nodiscard]] auto hot_record_accounted_bytes(const std::size_t key_bytes, const std::size_t value_bytes)
    -> Result<std::uint64_t> {
    return detail::hot_record_accounted_bytes(key_bytes, value_bytes);
}

void subtract_hot_record_accounting(std::uint64_t& total, const std::string_view key,
                                    const HotRecordEntry& entry) noexcept {
    const auto charge = detail::hot_record_accounted_bytes_saturated(key.size(), entry.value_size);
    // A resident entry was admitted only after this exact calculation
    // succeeded. Saturation keeps telemetry safe if that invariant is ever
    // violated instead of wrapping the budget counter.
    if (charge >= total) {
        total = 0;
        return;
    }
    total -= charge;
}

[[nodiscard]] auto hot_cache_worker_budget(const std::size_t worker_index, const std::size_t worker_count,
                                           const DurableResourceLimits& limits) noexcept -> std::uint64_t {
    if (worker_count == 0) {
        return 0;
    }
    const auto count = static_cast<std::uint64_t>(worker_count);
    const auto base = limits.max_hot_cache_bytes / count;
    const auto remainder = limits.max_hot_cache_bytes % count;
    const auto partition = base + (static_cast<std::uint64_t>(worker_index) < remainder ? 1U : 0U);
    return std::min(partition, limits.max_hot_cache_bytes_per_worker);
}

[[nodiscard]] auto hot_record_matches(const HotRecordEntry& entry, const RecordRef& reference) noexcept
    -> bool {
    return entry.reference == reference;
}

[[nodiscard]] auto owned_value_from_hot(const HotRecordSnapshot& entry) -> OwnedValue {
    std::vector<std::byte> value(entry.value_size);
    if (entry.value_size != 0) {
        const auto bytes = entry.value_span();
        std::copy_n(bytes.begin(), entry.value_size, value.begin());
    }
    return {.bytes = std::move(value), .sequence = entry.sequence.value, .expire_at_ns = entry.expire_at_ns};
}

auto rotation_manifest(const Manifest& current, const ManifestSegmentEntry& old_active,
                       const DurableResourceLimits& limits) -> Result<Manifest> {
    if (current.manifest_generation == std::numeric_limits<std::uint64_t>::max() ||
        current.next_segment_id.value == std::numeric_limits<std::uint64_t>::max() ||
        current.segments.size() == kMaximumManifestSegmentCount) {
        return fail(ErrorCode::arithmetic_overflow, "durable rotation catalog space is exhausted");
    }
    if (auto resources = validate_durable_rotation_resources(current.segments.size(), limits); !resources) {
        return unexpected(resources.error());
    }
    Manifest next = current;
    auto found = std::lower_bound(next.segments.begin(), next.segments.end(), old_active.segment_id,
                                  [](const ManifestSegmentEntry& entry, const SegmentId id) {
                                      return entry.segment_id.value < id.value;
                                  });
    if (found == next.segments.end() || found->segment_id != old_active.segment_id ||
        found->role != ManifestSegmentRole::active) {
        return fail(ErrorCode::corrupted_data, "rotation source is not the catalog active Segment");
    }
    found->role = ManifestSegmentRole::sealed;
    next.segments.push_back({.segment_id = current.next_segment_id,
                             .generation = current.next_segment_generation,
                             .owner_worker = old_active.owner_worker,
                             .role = ManifestSegmentRole::active});
    ++next.manifest_generation;
    ++next.next_segment_id.value;
    return next;
}

auto complete_interrupted_rotation(DataDirectory& directory, const DurableResourceLimits& limits) -> Status {
    auto manifest = directory.read_manifest(limits.max_manifest_bytes);
    if (!manifest) {
        return unexpected(manifest.error());
    }
    if (auto resources = validate_durable_manifest_resources(*manifest, limits); !resources) {
        return resources;
    }
    auto audit = audit_data_directory(directory, *manifest);
    if (!audit) {
        return unexpected(audit.error());
    }

    const NamespaceIssue* prepared_orphan{};
    for (const auto& issue : audit->issues) {
        if (issue.kind == NamespaceIssueKind::stale_manifest_temporary ||
            issue.kind == NamespaceIssueKind::stale_segment_temporary) {
            continue;
        }
        const bool exact_prepared = issue.kind == NamespaceIssueKind::unlisted_segment &&
                                    issue.segment_id == manifest->next_segment_id &&
                                    issue.generation == manifest->next_segment_generation;
        if (!exact_prepared || prepared_orphan) {
            return validate_namespace_for_recovery(*audit);
        }
        prepared_orphan = &issue;
    }

    const ManifestSegmentEntry* sealed_active{};
    for (const auto& entry : manifest->segments) {
        if (entry.role != ManifestSegmentRole::active) {
            continue;
        }
        const SegmentHeaderIdentity identity{.store_id = manifest->store_id,
                                             .segment_id = entry.segment_id,
                                             .generation = entry.generation,
                                             .owner_worker = entry.owner_worker,
                                             .format_version = manifest->format_version};
        auto file = DurableSegmentFile::open(directory, identity, SegmentFileOpenMode::read_only);
        if (!file) {
            return unexpected(file.error());
        }
        if (file->selected_commit().commit.state == PersistedSegmentState::sealed) {
            if (sealed_active) {
                return fail(ErrorCode::corrupted_data,
                            "multiple interrupted Worker rotations require operator recovery");
            }
            sealed_active = &entry;
        }
    }
    if (!sealed_active) {
        if (prepared_orphan) {
            return fail(ErrorCode::corrupted_data,
                        "unlisted next Segment has no sealed-active rotation intent");
        }
        return {};
    }

    const SegmentHeaderIdentity replacement_identity{.store_id = manifest->store_id,
                                                     .segment_id = manifest->next_segment_id,
                                                     .generation = manifest->next_segment_generation,
                                                     .owner_worker = sealed_active->owner_worker,
                                                     .format_version = manifest->format_version};
    auto next = rotation_manifest(*manifest, *sealed_active, limits);
    if (!next) {
        return unexpected(next.error());
    }
    const auto next_manifest_bytes = durable_manifest_bytes(next->segments.size());
    if (!next_manifest_bytes) {
        return unexpected(next_manifest_bytes.error());
    }
    auto additional = *next_manifest_bytes;
    if (!prepared_orphan) {
        if (additional > std::numeric_limits<std::uint64_t>::max() - kSegmentSizeBytes) {
            return fail(ErrorCode::arithmetic_overflow,
                        "interrupted rotation free-space requirement overflow");
        }
        additional += kSegmentSizeBytes;
    }
    if (auto available = require_durable_available_space(directory, additional, limits); !available) {
        return available;
    }
    std::optional<DurableSegmentFile> replacement;
    if (prepared_orphan) {
        auto opened =
            DurableSegmentFile::open(directory, replacement_identity, SegmentFileOpenMode::read_write);
        if (!opened) {
            return unexpected(opened.error());
        }
        const auto& commit = opened->selected_commit().commit;
        if (commit.commit_generation != 1 || commit.record_count != 0 ||
            commit.committed_end != kSegmentHeaderReservedBytes ||
            commit.state != PersistedSegmentState::active) {
            return fail(ErrorCode::corrupted_data,
                        "prepared rotation replacement is not a pristine active Segment");
        }
        replacement.emplace(std::move(*opened));
    } else {
        auto created = DurableSegmentFile::create(directory, replacement_identity);
        if (!created.durable()) {
            return unexpected(
                created.error.value_or(Error{ErrorCode::io_error, "rotation replacement creation failed"}));
        }
        if (!created.file) {
            return fail(ErrorCode::internal_error, "durable rotation replacement has no file handle");
        }
        replacement.emplace(std::move(*created.file));
    }

    const auto published = directory.publish_manifest(*next, limits.max_manifest_bytes);
    if (!published.durable()) {
        return unexpected(
            published.error.value_or(Error{ErrorCode::io_error, "rotation manifest publication failed"}));
    }
    return {};
}

auto resolve_interrupted_compaction(DataDirectory& directory, const std::uint64_t recovery_now_ns,
                                    const DurableResourceLimits& limits)
    -> Result<std::optional<DurableRecoveryState>> {
    auto intent = directory.read_compaction_intent(limits.max_manifest_bytes);
    if (!intent) {
        if (intent.error().code == ErrorCode::not_found) {
            return std::optional<DurableRecoveryState>{};
        }
        return unexpected(intent.error());
    }
    auto authority = directory.read_manifest(limits.max_manifest_bytes);
    if (!authority) {
        return unexpected(authority.error());
    }
    const bool old_authority = *authority == intent->old_manifest;
    const bool next_authority = *authority == intent->next_manifest;
    if (!old_authority && !next_authority) {
        return fail(ErrorCode::corrupted_data, "compaction intent matches neither authoritative manifest");
    }

    auto recovered = recover_durable_state(directory, recovery_now_ns, limits, &*intent);
    if (!recovered) {
        return unexpected(recovered.error());
    }
    const auto replacement_count = validate_durable_compaction_transition(
        intent->old_manifest, intent->next_manifest, intent->worker_id);
    if (!replacement_count) {
        return unexpected(replacement_count.error());
    }
    std::vector<ManifestSegmentEntry> sources;
    std::vector<ManifestSegmentEntry> replacements;
    for (const auto& entry : intent->old_manifest.segments) {
        if (entry.owner_worker != intent->worker_id || entry.role != ManifestSegmentRole::sealed) {
            continue;
        }
        sources.push_back(entry);
        if (replacements.size() < *replacement_count) {
            auto replacement = entry;
            ++replacement.generation.value;
            replacements.push_back(replacement);
        }
    }
    const auto& obsolete = old_authority ? replacements : sources;
    const auto& obsolete_manifest = old_authority ? intent->next_manifest : intent->old_manifest;
    for (const auto& entry : obsolete) {
        const SegmentHeaderIdentity identity{.store_id = obsolete_manifest.store_id,
                                             .segment_id = entry.segment_id,
                                             .generation = entry.generation,
                                             .owner_worker = entry.owner_worker,
                                             .format_version = obsolete_manifest.format_version};
        auto file = DurableSegmentFile::open(directory, identity, SegmentFileOpenMode::read_only);
        if (!file && file.error().code != ErrorCode::not_found) {
            return unexpected(file.error());
        }
    }
    const auto retired = directory.retire_compaction_segments(obsolete_manifest.store_id, obsolete);
    if (!retired.durable()) {
        return unexpected(
            retired.error.value_or(Error{ErrorCode::io_error, "compaction Segment retirement failed"}));
    }
    const auto removed = directory.remove_compaction_intent();
    if (!removed.durable()) {
        return unexpected(
            removed.error.value_or(Error{ErrorCode::io_error, "compaction intent removal failed"}));
    }
    auto final_audit = audit_data_directory(directory, recovered->manifest);
    if (!final_audit) {
        return unexpected(final_audit.error());
    }
    if (auto safe = validate_namespace_for_recovery(*final_audit); !safe) {
        return unexpected(safe.error());
    }
    recovered->namespace_audit = std::move(*final_audit);
    return std::optional<DurableRecoveryState>{std::move(*recovered)};
}

auto rollback_prepared_compaction(DataDirectory& directory, const Manifest& old_manifest,
                                  const std::span<const ManifestSegmentEntry> replacements,
                                  const DurableResourceLimits& limits) -> Result<NamespaceAuditReport> {
    auto authority = directory.read_manifest(limits.max_manifest_bytes);
    if (!authority) {
        return unexpected(authority.error());
    }
    if (*authority != old_manifest) {
        return fail(ErrorCode::sequence_conflict,
                    "cannot roll back compaction after its old manifest lost authority");
    }
    const auto retired = directory.retire_compaction_segments(old_manifest.store_id, replacements);
    if (!retired.durable()) {
        return unexpected(
            retired.error.value_or(Error{ErrorCode::io_error, "compaction replacement rollback failed"}));
    }
    const auto removed = directory.remove_compaction_intent();
    if (!removed.durable()) {
        return unexpected(
            removed.error.value_or(Error{ErrorCode::io_error, "compaction intent rollback failed"}));
    }
    auto audit = audit_data_directory(directory, old_manifest);
    if (!audit) {
        return unexpected(audit.error());
    }
    if (auto safe = validate_namespace_for_recovery(*audit); !safe) {
        return unexpected(safe.error());
    }
    return audit;
}

} // namespace glifistore::runtime_catalog_detail
