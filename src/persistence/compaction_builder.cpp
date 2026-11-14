#include "glifistore/persistence/compaction_builder.hpp"

#include "glifistore/core/byte_rate_limiter.hpp"
#include "glifistore/core/key_hash.hpp"
#include "glifistore/persistence/compaction_intent.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "glifistore/persistence/segment_file.hpp"
#include "glifistore/segment/crc32c.hpp"
#include "glifistore/segment/record.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace glifistore {
namespace {

struct PackedRecord {
    std::size_t source_index{};
    DurableCompactionPlacement placement;
    RecordRef output{};
    std::uint32_t encoded_crc{};
};

static_assert(sizeof(PackedRecord) <= 64, "compaction placement metadata must remain within one cache line");

struct VerifiedRecordContext {
    std::string_view key;
    std::uint64_t key_hash{};
    std::size_t worker_count{};
    WorkerId worker_id{};
    std::uint64_t now_ns{};
    bool expired{};
    std::optional<BlobRef> blob{};
};

auto verify_indexed_put(void* opaque, const RecordView& record) -> Status {
    auto& context = *static_cast<VerifiedRecordContext*>(opaque);
    if (record.opcode != Opcode::put || record.key_hash != context.key_hash ||
        record.key_string() != context.key ||
        route_worker(record.key_hash, context.worker_count) != context.worker_id.value) {
        return fail(ErrorCode::corrupted_data,
                    "durable compaction Index entry disagrees with its source Record");
    }
    context.expired = record.expired(context.now_ns);
    if (record.type == ValueType::blob_ref) {
        auto ref = decode_blob_ref(record.value);
        if (!ref) {
            return unexpected(ref.error());
        }
        context.blob = *ref;
    }
    return {};
}

auto build_failure(const DurableCompactionBuildOutcome outcome, Error error,
                   DurableCompactionCopyStats stats = {}) -> DurableCompactionBuildResult {
    return {.outcome = outcome, .prepared = std::nullopt, .stats = stats, .error = std::move(error)};
}

auto checked_add(const std::uint64_t left, const std::uint64_t right, const char* description)
    -> Result<std::uint64_t> {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return fail(ErrorCode::arithmetic_overflow, description);
    }
    return left + right;
}

auto elapsed_ns(const std::chrono::steady_clock::time_point start) noexcept -> std::uint64_t {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
            .count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

auto steady_now_ns() noexcept -> std::uint64_t {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return nanoseconds > 0 ? static_cast<std::uint64_t>(nanoseconds) : 0U;
}

struct CompactionWritePacerContext final {
    BoundedByteRateLimiter* limiter{};
    DurableCompactionCopyStats* stats{};
};

auto acquire_compaction_write(void* opaque, const std::size_t requested_bytes) -> Result<std::size_t> {
    auto& context = *static_cast<CompactionWritePacerContext*>(opaque);
    if (context.limiter == nullptr || context.stats == nullptr || requested_bytes == 0U) {
        return fail(ErrorCode::internal_error, "compaction pacing context is invalid");
    }
    const auto decision = context.limiter->request(requested_bytes, steady_now_ns());
    if (decision.granted_bytes == 0U || decision.granted_bytes > requested_bytes ||
        decision.granted_bytes > std::numeric_limits<std::size_t>::max()) {
        return fail(ErrorCode::internal_error, "compaction pacing returned an invalid write grant");
    }
    if (decision.sleep_ns != 0U) {
        const auto sleep_started = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::nanoseconds{decision.sleep_ns});
        const auto slept = elapsed_ns(sleep_started);
        context.stats->pacing_delay_ns =
            slept > std::numeric_limits<std::uint64_t>::max() - context.stats->pacing_delay_ns
                ? std::numeric_limits<std::uint64_t>::max()
                : context.stats->pacing_delay_ns + slept;
        if (context.stats->pacing_sleep_count != std::numeric_limits<std::uint64_t>::max()) {
            ++context.stats->pacing_sleep_count;
        }
    }
    return static_cast<std::size_t>(decision.granted_bytes);
}

auto add_capacity_bytes(std::uint64_t& total, const std::size_t capacity, const std::size_t element_size)
    -> Status {
    const auto count = static_cast<std::uint64_t>(capacity);
    const auto width = static_cast<std::uint64_t>(element_size);
    if (width != 0 && count > std::numeric_limits<std::uint64_t>::max() / width) {
        return fail(ErrorCode::arithmetic_overflow,
                    "durable compaction transient metadata accounting overflow");
    }
    auto next =
        checked_add(total, count * width, "durable compaction transient metadata accounting overflow");
    if (!next) {
        return unexpected(next.error());
    }
    total = *next;
    return {};
}

auto segment_identity(const Manifest& manifest, const ManifestSegmentEntry& entry) -> SegmentHeaderIdentity {
    return {.store_id = manifest.store_id,
            .segment_id = entry.segment_id,
            .generation = entry.generation,
            .owner_worker = entry.owner_worker,
            .format_version = manifest.format_version};
}

auto find_catalog_entry(const Manifest& manifest, const SegmentId segment_id) -> const ManifestSegmentEntry* {
    const auto found =
        std::ranges::lower_bound(manifest.segments, segment_id, {}, &ManifestSegmentEntry::segment_id);
    return found != manifest.segments.end() && found->segment_id == segment_id ? &*found : nullptr;
}

auto find_source_index(const std::span<const ManifestSegmentEntry> sources, const SegmentId segment_id)
    -> std::optional<std::size_t> {
    const auto found = std::ranges::lower_bound(sources, segment_id, {}, &ManifestSegmentEntry::segment_id);
    if (found == sources.end() || found->segment_id != segment_id) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - sources.begin());
}

class StagedSegmentGuard final {
  public:
    StagedSegmentGuard(DataDirectory& directory, std::span<const SegmentHeaderIdentity> identities) noexcept
        : directory_(&directory), identities_(identities) {}

    ~StagedSegmentGuard() {
        if (directory_ != nullptr) {
            DurableSegmentFile::discard_staged(*directory_, identities_.first(created_count_));
        }
    }

    StagedSegmentGuard(const StagedSegmentGuard&) = delete;
    auto operator=(const StagedSegmentGuard&) -> StagedSegmentGuard& = delete;

    void mark_created() noexcept {
        if (created_count_ < identities_.size()) {
            ++created_count_;
        }
    }

    void release() noexcept {
        directory_ = nullptr;
    }

  private:
    DataDirectory* directory_{};
    std::span<const SegmentHeaderIdentity> identities_{};
    std::size_t created_count_{};
};

} // namespace

auto durable_compaction_prefix_entries(const Manifest& current, const WorkerId worker_id,
                                       const std::size_t source_count, std::vector<IndexEntry> entries)
    -> std::vector<IndexEntry> {
    std::vector<ManifestSegmentEntry> prefix;
    for (const auto& entry : current.segments) {
        if (entry.owner_worker == worker_id && entry.role == ManifestSegmentRole::sealed &&
            (source_count == 0 || prefix.size() < source_count)) {
            prefix.push_back(entry);
        }
    }
    std::erase_if(entries, [&](const IndexEntry& entry) {
        const auto source = find_source_index(prefix, entry.record.segment_id);
        return !source || prefix[*source].generation != entry.record.generation;
    });
    return entries;
}

auto build_durable_worker_compaction(DataDirectory& directory, const Manifest& current,
                                     const WorkerId worker_id, std::vector<IndexEntry> entries,
                                     const DurableResourceLimits& limits,
                                     const DurableCompactionBuildRequest& request)
    -> DurableCompactionBuildResult {
    const auto now_ns = request.now_ns;
    const auto& intent_gate = request.intent_gate;
    const auto& pacing = request.pacing;
    bool recovery_required{};
    DurableCompactionCopyStats copy_stats{};
    BoundedByteRateLimiter copy_rate_limiter{pacing.bytes_per_second};
    copy_stats.pacing_burst_bytes = copy_rate_limiter.burst_bytes();
    const auto build_started = std::chrono::steady_clock::now();
    const auto failure = [&](Error error) {
        return build_failure(recovery_required ? DurableCompactionBuildOutcome::recovery_required
                                               : DurableCompactionBuildOutcome::not_started,
                             std::move(error), copy_stats);
    };
    try {
        CompactionWritePacerContext write_pacer_context{
            .limiter = &copy_rate_limiter,
            .stats = &copy_stats,
        };
        const SegmentRecordWritePacing write_pacing = pacing.bytes_per_second == 0U
                                                          ? SegmentRecordWritePacing{}
                                                          : SegmentRecordWritePacing{
                                                                .context = &write_pacer_context,
                                                                .acquire = &acquire_compaction_write,
                                                            };
        if (!directory.healthy()) {
            return failure(
                Error{ErrorCode::io_error, "cannot build compaction through a poisoned data directory"});
        }
        if (auto valid = validate_durable_manifest_resources(current, limits); !valid) {
            return failure(valid.error());
        }
        if (worker_id.value >= current.worker_count) {
            return failure(
                Error{ErrorCode::invalid_argument, "durable compaction Worker is outside the manifest"});
        }
        auto authority = directory.read_manifest(limits.max_manifest_bytes);
        if (!authority) {
            return failure(authority.error());
        }
        if (authority->store_id != current.store_id ||
            authority->manifest_generation < current.manifest_generation) {
            return failure(Error{ErrorCode::sequence_conflict,
                                 "durable compaction manifest snapshot is no longer authoritative"});
        }
        auto existing_intent = directory.read_compaction_intent(limits.max_manifest_bytes);
        if (existing_intent) {
            return failure(Error{ErrorCode::sequence_conflict,
                                 "another durable compaction intent is already installed"});
        }
        if (existing_intent.error().code != ErrorCode::not_found) {
            return failure(existing_intent.error());
        }

        // ADR 0044: the oldest `source_count` sealed Segments of the Worker, in
        // Manifest (sequence) order; zero selects the complete sealed set.
        std::vector<ManifestSegmentEntry> sources;
        std::size_t sealed_count{};
        for (const auto& entry : current.segments) {
            if (entry.owner_worker != worker_id || entry.role != ManifestSegmentRole::sealed) {
                continue;
            }
            ++sealed_count;
            if (request.source_count == 0 || sources.size() < request.source_count) {
                sources.push_back(entry);
            }
        }
        if (sources.empty()) {
            return failure(Error{ErrorCode::not_found, "durable Worker has no sealed Segments to compact"});
        }
        if (request.source_count != 0 && sources.size() != request.source_count) {
            return failure(Error{ErrorCode::invalid_argument,
                                 "durable compaction prefix exceeds the Worker's sealed Segment set"});
        }
        copy_stats.source_segments = sources.size();
        copy_stats.retained_sealed_segments = sealed_count - sources.size();
        for (const auto& source : sources) {
            const auto* authoritative = find_catalog_entry(*authority, source.segment_id);
            if (authoritative == nullptr || *authoritative != source) {
                return failure(Error{ErrorCode::sequence_conflict,
                                     "durable compaction source is no longer authoritative"});
            }
        }

        std::map<BlobId, std::uint64_t> source_blobs;
        std::set<BlobId> retained_blobs;
        std::vector<SelectedSegmentCommit> source_commits;
        source_commits.reserve(sources.size());
        for (const auto& source : sources) {
            auto opened = DurableSegmentFile::open(directory, segment_identity(current, source),
                                                   SegmentFileOpenMode::read_only);
            if (!opened) {
                return failure(opened.error());
            }
            if (opened->selected_commit().commit.state != PersistedSegmentState::sealed) {
                return failure(Error{ErrorCode::corrupted_data, "durable compaction source is not sealed"});
            }
            if (current.format_version == 2) {
                auto count = [](void* context, const RecordRef&, const RecordView& record) -> Status {
                    if (record.type == ValueType::blob_ref) {
                        auto ref = decode_blob_ref(record.value);
                        if (!ref) {
                            return unexpected(ref.error());
                        }
                        auto& blobs = *static_cast<std::map<BlobId, std::uint64_t>*>(context);
                        if (blobs.size() >= 1'000'000) {
                            return fail(ErrorCode::resource_exhausted, "blob compaction metadata bound");
                        }
                        blobs.try_emplace(ref->id, blob_file_bytes(ref->logical_size));
                    }
                    return {};
                };
                if (auto scanned = opened->visit_committed_records(&source_blobs, count); !scanned) {
                    return failure(scanned.error());
                }
            }
            source_commits.push_back(opened->selected_commit());
        }

        const auto routing = current.worker_routing();
        std::vector<IndexEntry> source_entries;
        source_entries.reserve(entries.size());
        std::vector<DurableCompactionRemap> remaps;
        for (auto& entry : entries) {
            const auto key_hash = hash_key_routing(entry.key, routing);
            if (route_worker(key_hash, current.worker_count) != worker_id.value) {
                return failure(Error{ErrorCode::corrupted_data,
                                     "durable compaction Index routing or hash is inconsistent"});
            }
            const auto* catalog_entry = find_catalog_entry(current, entry.record.segment_id);
            if (catalog_entry == nullptr || catalog_entry->generation != entry.record.generation ||
                catalog_entry->owner_worker != worker_id ||
                !find_source_index(sources, entry.record.segment_id)) {
                return failure(Error{ErrorCode::corrupted_data,
                                     "durable compaction Index references an invalid catalog identity"});
            }
            source_entries.push_back(std::move(entry));
        }
        entries = {};

        std::ranges::sort(source_entries, [](const IndexEntry& left, const IndexEntry& right) {
            if (left.record.sequence != right.record.sequence) {
                return left.record.sequence.value < right.record.sequence.value;
            }
            if (left.record.segment_id != right.record.segment_id) {
                return left.record.segment_id.value < right.record.segment_id.value;
            }
            return left.record.offset.value < right.record.offset.value;
        });
        for (std::size_t index = 1; index < source_entries.size(); ++index) {
            const auto& previous = source_entries[index - 1U].record;
            const auto& next = source_entries[index].record;
            if (previous.sequence == next.sequence || previous.segment_id.value > next.segment_id.value ||
                (previous.segment_id == next.segment_id && previous.offset.value >= next.offset.value)) {
                return failure(Error{ErrorCode::corrupted_data,
                                     "durable compaction source Index order is inconsistent"});
            }
        }

        DurableCompactionLayout layout;
        std::vector<PackedRecord> packed;
        packed.reserve(source_entries.size());
        std::optional<std::size_t> open_source_index;
        std::optional<DurableSegmentFile> open_source;
        std::vector<std::byte> scratch;
        for (std::size_t entry_index = 0; entry_index < source_entries.size(); ++entry_index) {
            auto& entry = source_entries[entry_index];
            const auto source_index = find_source_index(sources, entry.record.segment_id);
            if (!source_index || sources[*source_index].generation != entry.record.generation) {
                return failure(Error{ErrorCode::corrupted_data,
                                     "durable compaction source reference is not in the sealed set"});
            }
            if (!open_source_index || *open_source_index != *source_index) {
                open_source.reset();
                auto opened =
                    DurableSegmentFile::open(directory, segment_identity(current, sources[*source_index]),
                                             SegmentFileOpenMode::read_only);
                if (!opened || opened->selected_commit() != source_commits[*source_index]) {
                    return failure(opened ? Error{ErrorCode::corrupted_data,
                                                  "durable compaction source boundary changed"}
                                          : opened.error());
                }
                open_source.emplace(std::move(*opened));
                open_source_index = source_index;
            }
            if (!open_source) {
                return failure(
                    Error{ErrorCode::internal_error, "durable compaction source handle was not installed"});
            }
            const auto key_hash = hash_key_routing(entry.key, routing);
            VerifiedRecordContext context{.key = entry.key,
                                          .key_hash = key_hash,
                                          .worker_count = current.worker_count,
                                          .worker_id = worker_id,
                                          .now_ns = now_ns};
            if (auto visited =
                    open_source.value().visit_record(entry.record, scratch, &context, &verify_indexed_put);
                !visited) {
                return failure(visited.error());
            }
            ++copy_stats.source_index_records_verified;
            copy_stats.source_bytes_verified += entry.record.size.value;
            if (context.expired) {
                ++copy_stats.expired_records_dropped;
                remaps.push_back({.key = std::move(entry.key),
                                  .key_hash = key_hash,
                                  .source = entry.record,
                                  .output = std::nullopt});
                continue;
            }
            if (context.blob) {
                retained_blobs.insert(context.blob->id);
            }
            auto placement = layout.add_record(entry.record.size.value);
            if (!placement) {
                return failure(placement.error());
            }
            packed.push_back(
                {.source_index = entry_index, .placement = *placement, .encoded_crc = crc32c(scratch)});
        }
        open_source.reset();
        open_source_index.reset();

        std::uint64_t blob_reclaim_bytes{};
        for (const auto& [id, bytes] : source_blobs) {
            if (!retained_blobs.contains(id)) {
                if (bytes > std::numeric_limits<std::uint64_t>::max() - blob_reclaim_bytes) {
                    return failure(Error{ErrorCode::arithmetic_overflow, "blob reclaim overflow"});
                }
                blob_reclaim_bytes += bytes;
            }
        }
        const bool blob_gain = current.format_version == 2 && blob_reclaim_bytes != 0;
        if (layout.segment_count() > sources.size() ||
            (layout.segment_count() == sources.size() && !blob_gain)) {
            copy_stats.pre_intent_duration_ns = elapsed_ns(build_started);
            return build_failure(
                DurableCompactionBuildOutcome::not_beneficial,
                Error{ErrorCode::not_found, "durable compaction would not reclaim a physical Segment"},
                copy_stats);
        }

        auto plan = plan_durable_worker_compaction(current, worker_id, layout.segment_count(), limits,
                                                   blob_reclaim_bytes, sources.size());
        if (!plan) {
            return failure(plan.error());
        }
        if (plan->sources != sources || plan->replacements.size() != layout.segment_count()) {
            return failure(Error{ErrorCode::internal_error,
                                 "durable compaction planner disagrees with the exact Record layout"});
        }
        for (auto& record : packed) {
            const auto& source = source_entries[record.source_index];
            const auto& replacement = plan->replacements[record.placement.segment_index];
            record.output = {.segment_id = replacement.segment_id,
                             .offset = record.placement.offset,
                             .size = source.record.size,
                             .sequence = source.record.sequence,
                             .generation = replacement.generation};
        }

        const DurableCompactionIntent intent{
            .worker_id = worker_id, .old_manifest = current, .next_manifest = plan->next_manifest};
        const auto intent_bytes = encoded_compaction_intent_size(intent);
        const auto next_manifest_bytes = encoded_manifest_size(plan->next_manifest);
        if (!intent_bytes || !next_manifest_bytes) {
            return failure((!intent_bytes ? intent_bytes.error() : next_manifest_bytes.error()));
        }
        auto additional = checked_add(plan->temporary_bytes, static_cast<std::uint64_t>(*intent_bytes),
                                      "durable compaction free-space requirement overflow");
        if (additional) {
            additional = checked_add(*additional, static_cast<std::uint64_t>(*next_manifest_bytes),
                                     "durable compaction free-space requirement overflow");
        }
        if (!additional) {
            return failure(additional.error());
        }
        if (auto available = require_durable_available_space(directory, *additional, limits); !available) {
            return failure(available.error());
        }

        std::vector<SegmentHeaderIdentity> staged_identities;
        staged_identities.reserve(plan->replacements.size());
        for (const auto& replacement : plan->replacements) {
            staged_identities.push_back(segment_identity(current, replacement));
        }
        StagedSegmentGuard staged_guard{directory, staged_identities};
        std::vector<SelectedSegmentCommit> replacement_commits;
        replacement_commits.reserve(plan->replacements.size());

        std::optional<DurableSegmentFile> output;
        std::optional<std::size_t> output_index;
        std::size_t output_group_begin{};
        // ADR 0046: outputs are mirrored while they are written so that live-only data is born
        // resident. Each output may borrow the image bytes its sources still hold.
        std::vector<std::shared_ptr<SegmentImage>> replacement_images;
        replacement_images.reserve(plan->replacements.size());
        std::shared_ptr<SegmentImage> output_image;
        const auto finalize_output = [&](const std::size_t group_end) -> Status {
            if (!output || !output_index || group_end <= output_group_begin) {
                return fail(ErrorCode::internal_error, "durable compaction output group is incomplete");
            }
            const auto sealed = output->seal();
            if (!sealed.committed()) {
                return unexpected(sealed.error.value_or(
                    Error{ErrorCode::io_error, "durable compaction replacement seal failed"}));
            }
            const auto selected = output->selected_commit();
            const auto& commit = selected.commit;
            const auto expected_count = group_end - output_group_begin;
            const auto expected_end = static_cast<std::uint32_t>(packed[group_end - 1U].output.offset.value +
                                                                 packed[group_end - 1U].output.size.value);
            if (commit.state != PersistedSegmentState::sealed || commit.record_count != expected_count ||
                commit.committed_end != expected_end ||
                commit.first_sequence != packed[output_group_begin].output.sequence ||
                commit.last_sequence != packed[group_end - 1U].output.sequence) {
                return fail(ErrorCode::corrupted_data,
                            "durable compaction replacement metadata failed validation");
            }
            for (auto index = output_group_begin; index < group_end; ++index) {
                const auto& record = packed[index];
                const auto& source = source_entries[record.source_index];
                VerifiedRecordContext context{.key = source.key,
                                              .key_hash = hash_key_routing(source.key, routing),
                                              .worker_count = current.worker_count,
                                              .worker_id = worker_id,
                                              .now_ns = now_ns};
                if (auto visited =
                        output->visit_record(record.output, scratch, &context, &verify_indexed_put);
                    !visited) {
                    return visited;
                }
                if (context.expired || crc32c(scratch) != record.encoded_crc) {
                    return fail(ErrorCode::checksum_mismatch,
                                "durable compaction replacement differs from its source Record");
                }
            }
            replacement_commits.push_back(selected);
            if (output_image) {
                output_image->seal(commit.committed_end);
            }
            replacement_images.push_back(std::move(output_image));
            output.reset();
            return {};
        };

        open_source.reset();
        open_source_index.reset();
        for (std::size_t index = 0; index < packed.size(); ++index) {
            const auto& record = packed[index];
            const auto& source = source_entries[record.source_index];
            if (!output_index || *output_index != record.placement.segment_index) {
                if (output) {
                    if (auto finalized = finalize_output(index); !finalized) {
                        return failure(finalized.error());
                    }
                }
                output_group_begin = index;
                output_index = record.placement.segment_index;
                auto created = DurableSegmentFile::create_staged(directory, staged_identities[*output_index]);
                if (!created) {
                    return failure(created.error());
                }
                staged_guard.mark_created();
                output.emplace(std::move(*created));
                if (request.image_reserve) {
                    output_image = request.image_credit
                                       ? request.image_credit->create_output(request.image_reserve)
                                       : SegmentImage::create(request.image_reserve);
                }
            }
            if (!output) {
                return failure(
                    Error{ErrorCode::internal_error, "durable compaction output handle was not installed"});
            }

            const auto source_index = find_source_index(sources, source.record.segment_id);
            if (!source_index) {
                return failure(Error{ErrorCode::internal_error, "durable compaction lost a source identity"});
            }
            if (!open_source_index || *open_source_index != *source_index) {
                open_source.reset();
                auto opened =
                    DurableSegmentFile::open(directory, segment_identity(current, sources[*source_index]),
                                             SegmentFileOpenMode::read_only);
                if (!opened || opened->selected_commit() != source_commits[*source_index]) {
                    return failure(opened ? Error{ErrorCode::corrupted_data,
                                                  "durable compaction source changed during copy"}
                                          : opened.error());
                }
                open_source.emplace(std::move(*opened));
                open_source_index = source_index;
            }
            if (!open_source) {
                return failure(
                    Error{ErrorCode::internal_error, "durable compaction source handle was not installed"});
            }
            VerifiedRecordContext context{.key = source.key,
                                          .key_hash = hash_key_routing(source.key, routing),
                                          .worker_count = current.worker_count,
                                          .worker_id = worker_id,
                                          .now_ns = now_ns};
            if (auto visited =
                    open_source.value().visit_record(source.record, scratch, &context, &verify_indexed_put);
                !visited) {
                return failure(visited.error());
            }
            if (context.expired || crc32c(scratch) != record.encoded_crc ||
                output.value().selected_commit().commit.committed_end != record.output.offset.value) {
                return failure(Error{ErrorCode::corrupted_data,
                                     "durable compaction source or output layout changed during copy"});
            }
            // Grants are acquired immediately before each bounded physical
            // write. The old Manifest remains the sole authority while this
            // sleeps, so pacing cannot lengthen an ambiguous recovery window.
            if (output_image) {
                output_image->mirror(record.output.offset.value, scratch);
            }
            const auto appended = output.value().append_record(scratch, write_pacing);
            if (!appended.committed()) {
                return failure(appended.error.value_or(
                    Error{ErrorCode::io_error, "durable compaction Record copy failed"}));
            }
            ++copy_stats.records_copied;
            copy_stats.bytes_copied += scratch.size();
        }
        if (output) {
            if (auto finalized = finalize_output(packed.size()); !finalized) {
                return failure(finalized.error());
            }
        }
        if (replacement_commits.size() != plan->replacements.size() ||
            copy_stats.records_copied != packed.size() || copy_stats.bytes_copied != layout.encoded_bytes()) {
            return failure(
                Error{ErrorCode::internal_error, "durable compaction replacement build is incomplete"});
        }

        if (auto accounted = add_capacity_bytes(copy_stats.transient_metadata_lower_bound_bytes,
                                                source_entries.capacity(), sizeof(IndexEntry));
            !accounted) {
            return failure(accounted.error());
        }
        if (auto accounted = add_capacity_bytes(copy_stats.transient_metadata_lower_bound_bytes,
                                                packed.capacity(), sizeof(PackedRecord));
            !accounted) {
            return failure(accounted.error());
        }
        if (auto accounted = add_capacity_bytes(copy_stats.transient_metadata_lower_bound_bytes,
                                                scratch.capacity(), sizeof(std::byte));
            !accounted) {
            return failure(accounted.error());
        }
        // Copied keys move into the remap list only after the copy and output
        // verification above, which still read them through source_entries.
        remaps.reserve(remaps.size() + packed.size());
        for (const auto& record : packed) {
            auto& source = source_entries[record.source_index];
            const auto key_hash = hash_key_routing(source.key, routing);
            remaps.push_back({.key = std::move(source.key),
                              .key_hash = key_hash,
                              .source = source.record,
                              .output = record.output});
        }
        source_entries = {};
        if (auto accounted = add_capacity_bytes(copy_stats.transient_metadata_lower_bound_bytes,
                                                remaps.capacity(), sizeof(DurableCompactionRemap));
            !accounted) {
            return failure(accounted.error());
        }
        copy_stats.pre_intent_duration_ns = elapsed_ns(build_started);

        // The expensive copy and full output verification are complete while
        // Mold is the only authority. The existing private temporary names are
        // non-authoritative and recovery-safe. Acquire the publication lease
        // only for intent -> promotion -> Manifest. ADR 0044: when rotations
        // advanced the authority meanwhile, rebase the identical prefix plan.
        Manifest old_authority = current;
        if (intent_gate.acquire != nullptr) {
            auto acquired = intent_gate.acquire(intent_gate.context);
            if (!acquired) {
                return failure(acquired.error());
            }
            old_authority = std::move(*acquired);
        }
        if (old_authority != current) {
            auto rebased = plan_durable_worker_compaction(old_authority, worker_id, plan->replacements.size(),
                                                          limits, blob_reclaim_bytes, sources.size());
            if (!rebased) {
                return failure(rebased.error());
            }
            if (rebased->sources != plan->sources || rebased->replacements != plan->replacements) {
                return failure(Error{ErrorCode::sequence_conflict,
                                     "durable compaction prefix changed before intent publication"});
            }
            plan = std::move(rebased);
            copy_stats.manifest_rebases = 1;
        }
        const DurableCompactionIntent rebased_intent{
            .worker_id = worker_id, .old_manifest = old_authority, .next_manifest = plan->next_manifest};
        const auto published = directory.publish_compaction_intent(rebased_intent, limits.max_manifest_bytes);
        if (!published.durable()) {
            recovery_required = published.outcome == CompactionIntentPublicationOutcome::indeterminate;
            return failure(published.error.value_or(
                Error{ErrorCode::io_error, "durable compaction intent publication failed"}));
        }
        recovery_required = true;
        if (auto promoted = DurableSegmentFile::promote_staged(directory, staged_identities); !promoted) {
            return failure(promoted.error());
        }
        staged_guard.release();

        return {.outcome = DurableCompactionBuildOutcome::prepared,
                .prepared = PreparedDurableCompaction{.plan = std::move(*plan),
                                                      .old_manifest = std::move(old_authority),
                                                      .remaps = std::move(remaps),
                                                      .replacement_commits = std::move(replacement_commits),
                                                      .replacement_images = std::move(replacement_images),
                                                      .stats = copy_stats},
                .error = std::nullopt};
    } catch (const std::bad_alloc&) {
        return failure(Error{ErrorCode::resource_exhausted, {}});
    } catch (...) {
        return failure(Error{ErrorCode::internal_error, {}});
    }
}

auto build_durable_worker_compaction(DataDirectory& directory, const Manifest& current,
                                     const WorkerId worker_id, const Index& current_index,
                                     const std::uint64_t now_ns, const DurableResourceLimits& limits)
    -> DurableCompactionBuildResult {
    const auto stats = current_index.stats();
    auto entries = current_index.entries();
    if (entries.size() != stats.size) {
        return build_failure(
            DurableCompactionBuildOutcome::not_started,
            Error{ErrorCode::corrupted_data, "durable compaction Index enumeration changed size"});
    }
    for (const auto& entry : entries) {
        const auto hashed = HashedKey::compute(entry.key);
        if (current_index.find(hashed) != entry.record) {
            return build_failure(
                DurableCompactionBuildOutcome::not_started,
                Error{ErrorCode::corrupted_data, "durable compaction Index snapshot is inconsistent"});
        }
    }
    if (worker_id.value >= current.worker_count) {
        return build_failure(
            DurableCompactionBuildOutcome::not_started,
            Error{ErrorCode::invalid_argument, "durable compaction Worker is outside the manifest"});
    }
    for (const auto& entry : entries) {
        const auto* catalog_entry = find_catalog_entry(current, entry.record.segment_id);
        if (catalog_entry == nullptr || catalog_entry->generation != entry.record.generation ||
            catalog_entry->owner_worker != worker_id) {
            return build_failure(DurableCompactionBuildOutcome::not_started,
                                 Error{ErrorCode::corrupted_data,
                                       "durable compaction Index references an invalid catalog identity"});
        }
    }
    return build_durable_worker_compaction(
        directory, current, worker_id,
        durable_compaction_prefix_entries(current, worker_id, 0, std::move(entries)), limits,
        {.now_ns = now_ns});
}

} // namespace glifistore
