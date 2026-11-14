#include "glifistore/persistence/compaction.hpp"
#include "glifistore/persistence/compaction_intent.hpp"
#include "glifistore/segment/crc32c.hpp"
#include "glifistore/segment/segment_header.hpp"
#include "hex_fixture.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>

namespace {

inline constexpr std::size_t kChecksumOffset = 56;

void put_u32(const std::span<std::byte> out, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        out[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void refresh_checksum(const std::span<std::byte> bytes) {
    put_u32(bytes, kChecksumOffset, 0);
    put_u32(bytes, kChecksumOffset, glifistore::crc32c(bytes));
}

auto intent_fixture() -> glifistore::DurableCompactionIntent {
    glifistore::Manifest old{
        .store_id = {std::byte{0x61}, std::byte{0x62}, std::byte{0x63}},
        .manifest_generation = 9,
        .worker_count = 1,
        .routing_epoch = 7,
        .next_segment_id = glifistore::SegmentId{5},
        .next_segment_generation = glifistore::GenerationId{1},
        .segments =
            {
                {.segment_id = glifistore::SegmentId{1},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{2},
                 .generation = glifistore::GenerationId{2},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{3},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::sealed},
                {.segment_id = glifistore::SegmentId{4},
                 .generation = glifistore::GenerationId{1},
                 .owner_worker = glifistore::WorkerId{0},
                 .role = glifistore::ManifestSegmentRole::active},
            },
    };
    auto limits = glifistore::DurableResourceLimits{};
    limits.max_store_bytes = 10ULL * glifistore::kSegmentSizeBytes;
    limits.max_segment_count = 10;
    limits.max_temporary_compaction_bytes = 4ULL * glifistore::kSegmentSizeBytes;
    const auto plan = glifistore::plan_durable_worker_compaction(old, glifistore::WorkerId{0}, 1, limits);
    GLIFI_REQUIRE(plan.has_value());
    return {.worker_id = glifistore::WorkerId{0},
            .old_manifest = std::move(old),
            .next_manifest = plan->next_manifest};
}

// The same old Manifest compacted as a strict prefix: sources 1 and 2 become output 1, and the sealed
// Segment 3 after the prefix is retained (ADR 0044, k = 2 < n = 3, m = 1).
auto prefix_intent_fixture() -> glifistore::DurableCompactionIntent {
    auto old = intent_fixture().old_manifest;
    auto limits = glifistore::DurableResourceLimits{};
    limits.max_store_bytes = 10ULL * glifistore::kSegmentSizeBytes;
    limits.max_segment_count = 10;
    limits.max_temporary_compaction_bytes = 4ULL * glifistore::kSegmentSizeBytes;
    const auto plan =
        glifistore::plan_durable_worker_compaction(old, glifistore::WorkerId{0}, 1, limits, 0, 2);
    GLIFI_REQUIRE(plan.has_value());
    return {.worker_id = glifistore::WorkerId{0},
            .old_manifest = std::move(old),
            .next_manifest = plan->next_manifest};
}

} // namespace

GLIFI_TEST("compaction intent v1 matches its independent golden fixture") {
    const auto intent = intent_fixture();
    const auto encoded = glifistore::encode_compaction_intent(intent);
    GLIFI_REQUIRE(encoded.has_value());
    const auto old_size = glifistore::encoded_manifest_size(intent.old_manifest);
    const auto next_size = glifistore::encoded_manifest_size(intent.next_manifest);
    GLIFI_REQUIRE(old_size.has_value());
    GLIFI_REQUIRE(next_size.has_value());
    GLIFI_REQUIRE(encoded->size() == glifistore::kCompactionIntentHeaderBytes + *old_size + *next_size);

    const auto fixture = glifistore::test::read_hex_fixture(std::filesystem::path{GLIFISTORE_SOURCE_DIR} /
                                                            "tests/fixtures/compaction_intent_v1.hex");
    GLIFI_REQUIRE(*encoded == fixture);

    const auto decoded = glifistore::decode_compaction_intent(fixture);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(*decoded == intent);
}

GLIFI_TEST("compaction intent rejects truncation trailing bytes and checksum corruption") {
    auto encoded = glifistore::encode_compaction_intent(intent_fixture());
    GLIFI_REQUIRE(encoded.has_value());

    auto decoded = glifistore::decode_compaction_intent(
        std::span<const std::byte>{*encoded}.first(glifistore::kCompactionIntentHeaderBytes - 1U));
    GLIFI_REQUIRE(!decoded.has_value());
    GLIFI_REQUIRE(decoded.error().code == glifistore::ErrorCode::invalid_record);

    encoded->push_back(std::byte{0});
    decoded = glifistore::decode_compaction_intent(*encoded);
    GLIFI_REQUIRE(!decoded.has_value());
    GLIFI_REQUIRE(decoded.error().code == glifistore::ErrorCode::invalid_record);
    encoded->pop_back();

    (*encoded)[glifistore::kCompactionIntentHeaderBytes] ^= std::byte{1};
    decoded = glifistore::decode_compaction_intent(*encoded);
    GLIFI_REQUIRE(!decoded.has_value());
    GLIFI_REQUIRE(decoded.error().code == glifistore::ErrorCode::checksum_mismatch);
}

GLIFI_TEST("compaction intent binds its header to canonical old and next manifests") {
    auto intent = intent_fixture();
    intent.next_manifest.next_segment_id.value += 1;
    auto encoded = glifistore::encode_compaction_intent(intent);
    GLIFI_REQUIRE(!encoded.has_value());
    GLIFI_REQUIRE(encoded.error().code == glifistore::ErrorCode::invalid_argument);

    encoded = glifistore::encode_compaction_intent(intent_fixture());
    GLIFI_REQUIRE(encoded.has_value());
    (*encoded)[16] ^= std::byte{1};
    refresh_checksum(*encoded);
    const auto decoded = glifistore::decode_compaction_intent(*encoded);
    GLIFI_REQUIRE(!decoded.has_value());
    GLIFI_REQUIRE(decoded.error().code == glifistore::ErrorCode::corrupted_data);
}

GLIFI_TEST("compaction intent fails closed for unknown versions and nonzero reserved bytes") {
    auto encoded = glifistore::encode_compaction_intent(intent_fixture());
    GLIFI_REQUIRE(encoded.has_value());
    (*encoded)[4] = std::byte{2};
    refresh_checksum(*encoded);
    auto decoded = glifistore::decode_compaction_intent(*encoded);
    GLIFI_REQUIRE(!decoded.has_value());
    GLIFI_REQUIRE(decoded.error().code == glifistore::ErrorCode::invalid_record);

    encoded = glifistore::encode_compaction_intent(intent_fixture());
    GLIFI_REQUIRE(encoded.has_value());
    (*encoded)[60] = std::byte{1};
    refresh_checksum(*encoded);
    decoded = glifistore::decode_compaction_intent(*encoded);
    GLIFI_REQUIRE(!decoded.has_value());
    GLIFI_REQUIRE(decoded.error().code == glifistore::ErrorCode::invalid_record);
}

GLIFI_TEST("compaction intent round-trips a strict sealed prefix and keeps its shape") {
    const auto whole = intent_fixture();
    const auto prefix = prefix_intent_fixture();
    const glifistore::DurableCompactionShape expected_shape{.source_count = 2, .replacement_count = 1};
    const auto shape = glifistore::derive_durable_compaction_shape(prefix.old_manifest, prefix.next_manifest,
                                                                   prefix.worker_id);
    GLIFI_REQUIRE(shape.has_value());
    GLIFI_REQUIRE(*shape == expected_shape);
    // The retained sealed Segment after the prefix is named by both authorities, unchanged.
    GLIFI_REQUIRE(prefix.next_manifest.segments.size() == 3);
    GLIFI_REQUIRE(prefix.next_manifest.segments[1] == prefix.old_manifest.segments[2]);
    GLIFI_REQUIRE(prefix.next_manifest.segments[2] == prefix.old_manifest.segments[3]);

    const auto encoded = glifistore::encode_compaction_intent(prefix);
    GLIFI_REQUIRE(encoded.has_value());
    const auto old_size = glifistore::encoded_manifest_size(prefix.old_manifest);
    const auto next_size = glifistore::encoded_manifest_size(prefix.next_manifest);
    GLIFI_REQUIRE(old_size.has_value() && next_size.has_value());
    GLIFI_REQUIRE(encoded->size() == glifistore::kCompactionIntentHeaderBytes + *old_size + *next_size);
    // One more Segment survives than in the whole-set transaction, which is exactly one Manifest entry.
    const auto whole_encoded = glifistore::encode_compaction_intent(whole);
    GLIFI_REQUIRE(whole_encoded.has_value());
    GLIFI_REQUIRE(encoded->size() == whole_encoded->size() + glifistore::kManifestSegmentEntryBytes);

    const auto decoded = glifistore::decode_compaction_intent(*encoded);
    GLIFI_REQUIRE(decoded.has_value());
    GLIFI_REQUIRE(*decoded == prefix);
    const auto decoded_shape = glifistore::derive_durable_compaction_shape(
        decoded->old_manifest, decoded->next_manifest, decoded->worker_id);
    GLIFI_REQUIRE(decoded_shape.has_value());
    GLIFI_REQUIRE(*decoded_shape == expected_shape);
}

GLIFI_TEST("compaction intent refuses non-canonical prefix transitions on encode") {
    constexpr auto sealed = glifistore::ManifestSegmentRole::sealed;
    const auto prefix = prefix_intent_fixture();
    const auto require_refused = [](const glifistore::DurableCompactionIntent& intent) {
        const auto size = glifistore::encoded_compaction_intent_size(intent);
        GLIFI_REQUIRE(!size.has_value());
        GLIFI_REQUIRE(size.error().code == glifistore::ErrorCode::invalid_argument);
        const auto encoded = glifistore::encode_compaction_intent(intent);
        GLIFI_REQUIRE(!encoded.has_value());
        GLIFI_REQUIRE(encoded.error().code == glifistore::ErrorCode::invalid_argument);
    };

    // The retained sealed Segment 3 must survive unchanged.
    auto bumped_retained = prefix;
    ++bumped_retained.next_manifest.segments[1].generation.value;
    require_refused(bumped_retained);

    // The output must reuse the earliest source ID with generation + 1.
    auto wrong_output = prefix;
    wrong_output.next_manifest.segments[0] = {.segment_id = glifistore::SegmentId{2},
                                              .generation = glifistore::GenerationId{3},
                                              .owner_worker = glifistore::WorkerId{0},
                                              .role = sealed};
    require_refused(wrong_output);
    auto skipped_generation = prefix;
    ++skipped_generation.next_manifest.segments[0].generation.value;
    require_refused(skipped_generation);

    // A middle Segment alone is not a prefix.
    auto middle_only = prefix;
    middle_only.next_manifest.segments = {prefix.old_manifest.segments[0], prefix.old_manifest.segments[1],
                                          prefix.old_manifest.segments[2], prefix.old_manifest.segments[3]};
    middle_only.next_manifest.segments[1].generation.value += 1U;
    require_refused(middle_only);
}

GLIFI_TEST("compaction intent decode fails closed on a valid but non-canonical prefix next manifest") {
    const auto prefix = prefix_intent_fixture();
    const auto encoded = glifistore::encode_compaction_intent(prefix);
    GLIFI_REQUIRE(encoded.has_value());

    // Same Segment count as the canonical next Manifest, but the output carries the ID of the later source
    // (2) instead of the earliest one (1). Manifest and intent checksums are valid, so only the transition
    // rule can refuse it.
    auto forged = prefix.next_manifest;
    forged.segments[0] = {.segment_id = glifistore::SegmentId{2},
                          .generation = glifistore::GenerationId{3},
                          .owner_worker = glifistore::WorkerId{0},
                          .role = glifistore::ManifestSegmentRole::sealed};
    const auto forged_bytes = glifistore::encode_manifest(forged);
    GLIFI_REQUIRE(forged_bytes.has_value());
    const auto old_size = glifistore::encoded_manifest_size(prefix.old_manifest);
    const auto next_size = glifistore::encoded_manifest_size(prefix.next_manifest);
    GLIFI_REQUIRE(old_size.has_value() && next_size.has_value());
    GLIFI_REQUIRE(forged_bytes->size() == *next_size);

    auto tampered = *encoded;
    std::copy(forged_bytes->begin(), forged_bytes->end(),
              tampered.begin() +
                  static_cast<std::ptrdiff_t>(glifistore::kCompactionIntentHeaderBytes + *old_size));
    refresh_checksum(tampered);
    const auto decoded = glifistore::decode_compaction_intent(tampered);
    GLIFI_REQUIRE(!decoded.has_value());
    GLIFI_REQUIRE(decoded.error().code == glifistore::ErrorCode::corrupted_data);

    // Control: the same splice with the canonical bytes decodes, so the refusal above is the transition rule.
    const auto canonical_bytes = glifistore::encode_manifest(prefix.next_manifest);
    GLIFI_REQUIRE(canonical_bytes.has_value());
    auto spliced = *encoded;
    std::copy(canonical_bytes->begin(), canonical_bytes->end(),
              spliced.begin() +
                  static_cast<std::ptrdiff_t>(glifistore::kCompactionIntentHeaderBytes + *old_size));
    refresh_checksum(spliced);
    GLIFI_REQUIRE(glifistore::decode_compaction_intent(spliced).has_value());
}
