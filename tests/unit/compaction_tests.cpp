#include "glifistore/persistence/compaction.hpp"
#include "glifistore/segment/segment_header.hpp"
#include "test.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

auto compaction_manifest() -> glifistore::Manifest {
    return {
        .store_id = {std::byte{0x47}},
        .manifest_generation = 7,
        .worker_count = 2,
        .routing_epoch = 1,
        .next_segment_id = glifistore::SegmentId{7},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments =
            {
                {.segment_id = glifistore::SegmentId{1},
                 .generation = glifistore::GenerationId{2},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{2},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{1},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{3},
                 .generation = glifistore::GenerationId{4},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{4},
                 .generation = glifistore::GenerationId{3},
                 .owner_worker = glifistore::WorkerId{1},
                 .role = glifistore::ManifestSegmentRole::active},
                {.segment_id = glifistore::SegmentId{5},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{6},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::active},
            },
    };
}

auto compaction_limits() -> glifistore::DurableResourceLimits {
    auto limits = glifistore::DurableResourceLimits{};
    limits.max_store_bytes = 16ULL * glifistore::kSegmentSizeBytes;
    limits.max_segment_count = 16;
    limits.max_temporary_compaction_bytes = 8ULL * glifistore::kSegmentSizeBytes;
    return limits;
}

} // namespace

GLIFI_TEST("durable compaction output sizing accounts for reserved Segment headers") {
    constexpr auto kPayload =
        static_cast<std::uint64_t>(glifistore::kSegmentSizeBytes - glifistore::kSegmentHeaderReservedBytes);
    GLIFI_REQUIRE(*glifistore::durable_compaction_output_segments(0) == 0);
    GLIFI_REQUIRE(*glifistore::durable_compaction_output_segments(1) == 1);
    GLIFI_REQUIRE(*glifistore::durable_compaction_output_segments(kPayload) == 1);
    GLIFI_REQUIRE(*glifistore::durable_compaction_output_segments(kPayload + 1) == 2);
}

GLIFI_TEST("durable compaction exact layout accounts for Record boundary fragmentation") {
    constexpr auto kRecords = std::size_t{127};
    glifistore::DurableCompactionLayout layout;
    for (std::size_t index = 0; index < kRecords; ++index) {
        const auto placement = layout.add_record(glifistore::kMaxNormalRecordSize);
        GLIFI_REQUIRE(placement.has_value());
    }
    const auto aggregate = static_cast<std::uint64_t>(kRecords) * glifistore::kMaxNormalRecordSize;
    GLIFI_REQUIRE(*glifistore::durable_compaction_output_segments(aggregate) == 2);
    GLIFI_REQUIRE(layout.segment_count() == 3);
    GLIFI_REQUIRE(layout.encoded_bytes() == aggregate);
}

GLIFI_TEST("durable compaction replaces a complete Worker sealed set in one manifest") {
    const auto current = compaction_manifest();
    const auto plan =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, compaction_limits());
    GLIFI_REQUIRE(plan.has_value());
    GLIFI_REQUIRE(plan->sources.size() == 3);
    GLIFI_REQUIRE(plan->replacements.size() == 1);
    GLIFI_REQUIRE(plan->replacements[0].segment_id == glifistore::SegmentId{1});
    GLIFI_REQUIRE(plan->replacements[0].generation == glifistore::GenerationId{3});
    GLIFI_REQUIRE(plan->next_manifest.manifest_generation == current.manifest_generation + 1);
    GLIFI_REQUIRE(plan->next_manifest.next_segment_id == current.next_segment_id);
    GLIFI_REQUIRE(plan->next_manifest.segments.size() == 4);
    GLIFI_REQUIRE(plan->next_manifest.segments[0] == plan->replacements[0]);
    GLIFI_REQUIRE(plan->next_manifest.segments[1] == current.segments[1]);
    GLIFI_REQUIRE(plan->next_manifest.segments[2] == current.segments[3]);
    GLIFI_REQUIRE(plan->next_manifest.segments[3] == current.segments[5]);
    GLIFI_REQUIRE(plan->temporary_bytes == glifistore::kSegmentSizeBytes);
    GLIFI_REQUIRE(plan->reclaimed_bytes == 2ULL * glifistore::kSegmentSizeBytes);
    GLIFI_REQUIRE(glifistore::encode_manifest(plan->next_manifest).has_value());
}

GLIFI_TEST("empty durable compaction can retire an entirely obsolete sealed history") {
    const auto current = compaction_manifest();
    const auto plan =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 0, compaction_limits());
    GLIFI_REQUIRE(plan.has_value());
    GLIFI_REQUIRE(plan->replacements.empty());
    GLIFI_REQUIRE(plan->next_manifest.segments.size() == 3);
    GLIFI_REQUIRE(plan->reclaimed_bytes == 3ULL * glifistore::kSegmentSizeBytes);
    GLIFI_REQUIRE(glifistore::encode_manifest(plan->next_manifest).has_value());
}

GLIFI_TEST("durable compaction rejects unsafe identity and non-reclaiming plans") {
    auto current = compaction_manifest();
    auto limits = compaction_limits();

    auto result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{2}, 0, limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::invalid_argument);

    result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{1}, 0, limits);
    GLIFI_REQUIRE(result.has_value());

    result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 4, limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::invalid_argument);

    result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 3, limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::storage_exhausted);

    current.manifest_generation = std::numeric_limits<std::uint64_t>::max();
    result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::arithmetic_overflow);

    current = compaction_manifest();
    current.segments[0].generation = glifistore::GenerationId{std::numeric_limits<std::uint32_t>::max()};
    result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::arithmetic_overflow);
}

GLIFI_TEST("durable compaction enforces temporary peak and amplification budgets") {
    const auto current = compaction_manifest();
    auto limits = compaction_limits();
    limits.max_temporary_compaction_bytes = glifistore::kSegmentSizeBytes;
    auto result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 2, limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::storage_exhausted);

    limits = compaction_limits();
    limits.max_write_amplification = 1;
    result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 2, limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::storage_exhausted);

    limits.max_write_amplification = 2;
    result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 2, limits);
    GLIFI_REQUIRE(result.has_value());

    limits = compaction_limits();
    limits.max_store_bytes = 7ULL * glifistore::kSegmentSizeBytes;
    limits.max_temporary_compaction_bytes = limits.max_store_bytes;
    result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::storage_exhausted);

    limits = compaction_limits();
    const auto current_manifest_bytes = glifistore::encoded_manifest_size(current);
    GLIFI_REQUIRE(current_manifest_bytes.has_value());
    const auto one_output =
        glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, limits);
    GLIFI_REQUIRE(one_output.has_value());
    const auto next_manifest_bytes = glifistore::encoded_manifest_size(one_output->next_manifest);
    GLIFI_REQUIRE(next_manifest_bytes.has_value());
    limits.max_store_bytes = current.segments.size() * glifistore::kSegmentSizeBytes +
                             glifistore::kSegmentSizeBytes + *current_manifest_bytes + *next_manifest_bytes;
    limits.max_temporary_compaction_bytes = limits.max_store_bytes;
    result = glifistore::plan_durable_worker_compaction(current, glifistore::WorkerId{0}, 1, limits);
    GLIFI_REQUIRE(!result.has_value());
    GLIFI_REQUIRE(result.error().code == glifistore::ErrorCode::storage_exhausted);
}
