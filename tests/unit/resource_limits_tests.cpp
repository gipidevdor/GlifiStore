#include "glifistore/core/types.hpp"
#include "glifistore/persistence/resource_limits.hpp"
#include "persistence/system_error.hpp"
#include "test.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace {

auto manifest_with_segments(const std::size_t segment_count, const std::size_t worker_count = 1)
    -> glifistore::Manifest {
    glifistore::Manifest manifest{
        .store_id = {std::byte{0x31}},
        .manifest_generation = 1,
        .worker_count = static_cast<std::uint32_t>(worker_count),
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{segment_count + 1U},
        .next_segment_generation = glifistore::GenerationId{1},
    };
    manifest.segments.reserve(segment_count);
    for (std::size_t index = 0; index < segment_count; ++index) {
        manifest.segments.push_back({
            .segment_id = glifistore::SegmentId{index + 1U},
            .generation = glifistore::GenerationId{1},
            .owner_worker = glifistore::WorkerId{static_cast<std::uint32_t>(index % worker_count)},
            .role = index < worker_count ? glifistore::ManifestSegmentRole::active
                                         : glifistore::ManifestSegmentRole::sealed,
        });
    }
    return manifest;
}

void require_invalid(const glifistore::DurableResourceLimits& limits) {
    const auto result = glifistore::validate_durable_resource_limits(limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::invalid_argument);
}

} // namespace

GLIFI_TEST("durable resource limit configuration rejects every invalid boundary") {
    glifistore::DurableResourceLimits limits{};
    limits.max_store_bytes = glifistore::kSegmentSizeBytes - 1U;
    require_invalid(limits);

    limits = {};
    limits.max_segment_count = 0;
    require_invalid(limits);

    limits = {};
    limits.max_manifest_bytes = glifistore::kManifestHeaderBytes;
    require_invalid(limits);

    limits = {};
    limits.max_open_files = 3;
    require_invalid(limits);

    limits = {};
    limits.max_recovery_memory_bytes = 0;
    require_invalid(limits);

    limits = {};
    limits.max_live_keys = 0;
    require_invalid(limits);

    limits = {};
    limits.max_temporary_compaction_bytes = 0;
    require_invalid(limits);

    limits = {};
    limits.max_temporary_compaction_bytes = limits.max_store_bytes + 1U;
    require_invalid(limits);

    limits = {};
    limits.max_write_amplification = 0;
    require_invalid(limits);
    limits.max_write_amplification = 65;
    require_invalid(limits);
    limits = {};
    limits.max_compaction_delta_entries = 65'537;
    require_invalid(limits);
    limits = {};
    limits.max_compaction_delta_key_bytes = 64ULL * 1024ULL * 1024ULL + 1;
    require_invalid(limits);
}

GLIFI_TEST("durable bootstrap and rotation budgets account for peak namespace bytes") {
    const glifistore::DurableResourceLimits defaults{};
    const auto too_many_workers = glifistore::validate_durable_bootstrap_resources(256, defaults);
    GLIFI_REQUIRE(!too_many_workers.has_value());
    GLIFI_REQUIRE(too_many_workers.error().code == glifistore::ErrorCode::storage_exhausted);

    auto one_segment_only = defaults;
    one_segment_only.max_segment_count = 1;
    one_segment_only.max_store_bytes = 2ULL * glifistore::kSegmentSizeBytes;
    one_segment_only.max_temporary_compaction_bytes = glifistore::kSegmentSizeBytes;
    const auto rotation = glifistore::validate_durable_rotation_resources(1, one_segment_only);
    GLIFI_REQUIRE(!rotation.has_value());
    GLIFI_REQUIRE(rotation.error().code == glifistore::ErrorCode::storage_exhausted);

    auto exact_store = defaults;
    exact_store.max_segment_count = 1;
    exact_store.max_store_bytes = glifistore::kSegmentSizeBytes;
    exact_store.max_temporary_compaction_bytes = glifistore::kSegmentSizeBytes;
    const auto manifest =
        glifistore::validate_durable_manifest_resources(manifest_with_segments(1), exact_store);
    GLIFI_REQUIRE(!manifest.has_value());
    GLIFI_REQUIRE(manifest.error().code == glifistore::ErrorCode::storage_exhausted);
}

GLIFI_TEST("manifest descriptor and live-key budgets fail with stable categories") {
    auto limits = glifistore::DurableResourceLimits{};
    limits.max_segment_count = 2;
    limits.max_manifest_bytes = glifistore::kManifestHeaderBytes + glifistore::kManifestSegmentEntryBytes;
    auto result = glifistore::validate_durable_manifest_resources(manifest_with_segments(2), limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::storage_exhausted);

    limits = {};
    limits.max_open_files = 4;
    result = glifistore::validate_durable_manifest_resources(manifest_with_segments(1), limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::descriptor_exhausted);

    limits = {};
    limits.max_live_keys = 1;
    result = glifistore::validate_durable_manifest_resources(manifest_with_segments(2, 2), limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::resource_exhausted);
}

GLIFI_TEST("live-key partitions exactly preserve the configured global limit") {
    constexpr std::size_t kWorkers = 7;
    constexpr std::size_t kKeys = 103;
    std::size_t sum{};
    for (std::size_t worker = 0; worker < kWorkers; ++worker) {
        const auto limit = glifistore::durable_worker_live_key_limit(worker, kWorkers, kKeys);
        GLIFI_REQUIRE(limit == 14 || limit == 15);
        sum += limit;
    }
    GLIFI_REQUIRE(sum == kKeys);
    GLIFI_REQUIRE(glifistore::durable_worker_live_key_limit(kWorkers, kWorkers, kKeys) == 0);
}

GLIFI_TEST("persistence errno mapping preserves resource failure categories") {
    GLIFI_REQUIRE(glifistore::persistence_system_error("test", ENOSPC).error.code ==
                  glifistore::ErrorCode::storage_exhausted);
#if defined(EDQUOT)
    GLIFI_REQUIRE(glifistore::persistence_system_error("test", EDQUOT).error.code ==
                  glifistore::ErrorCode::storage_exhausted);
#endif
    GLIFI_REQUIRE(glifistore::persistence_system_error("test", EFBIG).error.code ==
                  glifistore::ErrorCode::file_too_large);
    GLIFI_REQUIRE(glifistore::persistence_system_error("test", EMFILE).error.code ==
                  glifistore::ErrorCode::descriptor_exhausted);
    GLIFI_REQUIRE(glifistore::persistence_system_error("test", ENFILE).error.code ==
                  glifistore::ErrorCode::descriptor_exhausted);
    GLIFI_REQUIRE(glifistore::persistence_system_error("test", EROFS).error.code ==
                  glifistore::ErrorCode::read_only_filesystem);
    GLIFI_REQUIRE(glifistore::persistence_system_error("test", EIO).error.code ==
                  glifistore::ErrorCode::io_error);
}
