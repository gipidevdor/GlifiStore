#include "glifistore/persistence/resource_limits.hpp"

#include "glifistore/core/types.hpp"
#include "glifistore/persistence/filesystem.hpp"
#include "system_error.hpp"

#include <limits>
#include <sys/resource.h>

namespace glifistore {
namespace {

[[nodiscard]] auto checked_add(const std::uint64_t left, const std::uint64_t right, const char* description)
    -> Result<std::uint64_t> {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return fail(ErrorCode::arithmetic_overflow, description);
    }
    return left + right;
}

[[nodiscard]] auto checked_multiply(const std::uint64_t left, const std::uint64_t right,
                                    const char* description) -> Result<std::uint64_t> {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return fail(ErrorCode::arithmetic_overflow, description);
    }
    return left * right;
}

[[nodiscard]] auto segment_bytes(const std::size_t count) -> Result<std::uint64_t> {
    return checked_multiply(static_cast<std::uint64_t>(count), static_cast<std::uint64_t>(kSegmentSizeBytes),
                            "durable Segment byte budget overflow");
}

[[nodiscard]] auto validate_descriptor_budget(const std::size_t worker_count, const std::size_t segment_count,
                                              const DurableResourceLimits& limits) -> Status {
    constexpr auto kNonWorkerDescriptors = std::size_t{4};
    if (worker_count > std::numeric_limits<std::size_t>::max() - segment_count ||
        worker_count + segment_count > std::numeric_limits<std::size_t>::max() - kNonWorkerDescriptors) {
        return fail(ErrorCode::arithmetic_overflow, "durable descriptor requirement overflow");
    }
    // Runtime keeps one immutable read pin per catalog Segment and may also
    // keep one mutable active handle per Worker. Cold reads reuse the pinned
    // descriptors, so request concurrency does not grow this requirement.
    const auto required = worker_count + segment_count + kNonWorkerDescriptors;
    if (required > limits.max_open_files) {
        return fail(ErrorCode::descriptor_exhausted,
                    "durable Store Worker count exceeds the configured open-file budget");
    }
    struct rlimit process_limit{};
    if (::getrlimit(RLIMIT_NOFILE, &process_limit) != 0) {
        return persistence_system_error("getrlimit(RLIMIT_NOFILE)");
    }
    if (process_limit.rlim_cur != RLIM_INFINITY &&
        static_cast<std::uintmax_t>(required) > static_cast<std::uintmax_t>(process_limit.rlim_cur)) {
        return fail(ErrorCode::descriptor_exhausted,
                    "durable Store descriptor requirement exceeds RLIMIT_NOFILE");
    }
    return {};
}

[[nodiscard]] auto validate_count_and_manifest(const std::size_t segment_count,
                                               const DurableResourceLimits& limits) -> Result<std::uint64_t> {
    if (segment_count == 0 || segment_count > limits.max_segment_count) {
        return fail(ErrorCode::storage_exhausted, "durable Segment count exceeds the configured budget");
    }
    auto manifest_bytes = durable_manifest_bytes(segment_count);
    if (!manifest_bytes) {
        return unexpected(manifest_bytes.error());
    }
    if (*manifest_bytes > limits.max_manifest_bytes) {
        return fail(ErrorCode::storage_exhausted, "durable manifest exceeds the configured byte budget");
    }
    return *manifest_bytes;
}

} // namespace

auto validate_durable_resource_limits(const DurableResourceLimits& limits) -> Status {
    if (limits.max_store_bytes < kSegmentSizeBytes || limits.max_segment_count == 0 ||
        limits.max_segment_count > kMaximumManifestSegmentCount ||
        limits.max_manifest_bytes < kManifestHeaderBytes + kManifestSegmentEntryBytes ||
        limits.max_manifest_bytes > kMaximumManifestBytes || limits.max_open_files < 4 ||
        limits.max_recovery_memory_bytes == 0 || limits.max_live_keys == 0 ||
        limits.max_temporary_compaction_bytes == 0 ||
        limits.max_temporary_compaction_bytes > limits.max_store_bytes ||
        limits.max_write_amplification == 0 || limits.max_write_amplification > 64 ||
        limits.max_hot_cache_value_bytes > kMaxNormalRecordSize ||
        limits.max_compaction_delta_entries > 65'536 ||
        limits.max_compaction_delta_key_bytes > 64ULL * 1024ULL * 1024ULL) {
        return fail(ErrorCode::invalid_argument, "durable resource limits are inconsistent");
    }
    return {};
}

auto durable_manifest_bytes(const std::size_t segment_count) -> Result<std::uint64_t> {
    if (segment_count > kMaximumManifestSegmentCount) {
        return fail(ErrorCode::arithmetic_overflow, "durable manifest Segment count is exhausted");
    }
    const auto entries = checked_multiply(static_cast<std::uint64_t>(segment_count),
                                          static_cast<std::uint64_t>(kManifestSegmentEntryBytes),
                                          "durable manifest byte size overflow");
    if (!entries) {
        return unexpected(entries.error());
    }
    return checked_add(kManifestHeaderBytes, *entries, "durable manifest byte size overflow");
}

auto validate_durable_bootstrap_resources(const std::size_t worker_count, const DurableResourceLimits& limits)
    -> Status {
    if (auto valid = validate_durable_resource_limits(limits); !valid) {
        return valid;
    }
    auto manifest_bytes = validate_count_and_manifest(worker_count, limits);
    if (!manifest_bytes) {
        return unexpected(manifest_bytes.error());
    }
    auto segments = segment_bytes(worker_count);
    if (!segments) {
        return unexpected(segments.error());
    }
    auto peak = checked_add(*segments, *manifest_bytes, "durable bootstrap peak byte budget overflow");
    if (!peak) {
        return unexpected(peak.error());
    }
    peak = checked_add(*peak, *manifest_bytes, "durable bootstrap peak byte budget overflow");
    if (!peak) {
        return unexpected(peak.error());
    }
    if (*peak > limits.max_store_bytes) {
        return fail(ErrorCode::storage_exhausted,
                    "durable bootstrap exceeds the configured Store byte budget");
    }
    if (limits.max_live_keys < worker_count) {
        return fail(ErrorCode::resource_exhausted,
                    "durable live-key budget cannot provide one partition per Worker");
    }
    return validate_descriptor_budget(worker_count, worker_count, limits);
}

auto validate_durable_manifest_resources(const Manifest& manifest, const DurableResourceLimits& limits)
    -> Status {
    if (auto valid = validate_durable_resource_limits(limits); !valid) {
        return valid;
    }
    auto manifest_bytes = validate_count_and_manifest(manifest.segments.size(), limits);
    if (!manifest_bytes) {
        return unexpected(manifest_bytes.error());
    }
    auto segments = segment_bytes(manifest.segments.size());
    if (!segments) {
        return unexpected(segments.error());
    }
    auto total = checked_add(*segments, *manifest_bytes, "durable Store byte budget overflow");
    if (!total) {
        return unexpected(total.error());
    }
    if (*total > limits.max_store_bytes) {
        return fail(ErrorCode::storage_exhausted, "durable Store exceeds the configured byte budget");
    }
    if (limits.max_live_keys < manifest.worker_count) {
        return fail(ErrorCode::resource_exhausted,
                    "durable live-key budget cannot provide one partition per Worker");
    }
    return validate_descriptor_budget(manifest.worker_count, manifest.segments.size(), limits);
}

auto validate_durable_rotation_resources(const std::size_t current_segment_count,
                                         const DurableResourceLimits& limits) -> Status {
    if (current_segment_count == std::numeric_limits<std::size_t>::max()) {
        return fail(ErrorCode::arithmetic_overflow, "durable rotation Segment count overflow");
    }
    const auto next_segment_count = current_segment_count + 1U;
    auto current_manifest = durable_manifest_bytes(current_segment_count);
    if (!current_manifest) {
        return unexpected(current_manifest.error());
    }
    auto next_manifest = validate_count_and_manifest(next_segment_count, limits);
    if (!next_manifest) {
        return unexpected(next_manifest.error());
    }
    auto segments = segment_bytes(next_segment_count);
    if (!segments) {
        return unexpected(segments.error());
    }
    auto peak = checked_add(*segments, *current_manifest, "durable rotation peak byte budget overflow");
    if (!peak) {
        return unexpected(peak.error());
    }
    peak = checked_add(*peak, *next_manifest, "durable rotation peak byte budget overflow");
    if (!peak) {
        return unexpected(peak.error());
    }
    if (*peak > limits.max_store_bytes) {
        return fail(ErrorCode::storage_exhausted,
                    "durable rotation exceeds the configured Store byte budget");
    }
    return {};
}

auto require_durable_available_space(const DataDirectory& directory, const std::uint64_t additional_bytes,
                                     const DurableResourceLimits& limits) -> Status {
    const auto required =
        checked_add(additional_bytes, limits.reserved_free_bytes, "durable free-space requirement overflow");
    if (!required) {
        return unexpected(required.error());
    }
    const auto available = directory.available_space_bytes();
    if (!available) {
        return unexpected(available.error());
    }
    if (*available < *required) {
        return fail(ErrorCode::storage_exhausted,
                    "data directory has insufficient available space for the configured reserve");
    }
    return {};
}

auto durable_worker_live_key_limit(const std::size_t worker_index, const std::size_t worker_count,
                                   const std::size_t total_limit) noexcept -> std::size_t {
    if (worker_count == 0 || worker_index >= worker_count) {
        return 0;
    }
    return total_limit / worker_count + (worker_index < total_limit % worker_count ? 1U : 0U);
}

} // namespace glifistore
