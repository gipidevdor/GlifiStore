#include "glifistore/persistence/runtime_catalog.hpp"
#include "persistence_recovery_test_support.hpp"

#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

const glifistore::ManifestSegmentEntry kSealedOne{.segment_id = glifistore::SegmentId{1},
                                                  .generation = glifistore::GenerationId{1},
                                                  .owner_worker = glifistore::WorkerId{0},
                                                  .role = glifistore::ManifestSegmentRole::sealed};
const glifistore::ManifestSegmentEntry kSealedTwo{.segment_id = glifistore::SegmentId{2},
                                                  .generation = glifistore::GenerationId{1},
                                                  .owner_worker = glifistore::WorkerId{0},
                                                  .role = glifistore::ManifestSegmentRole::sealed};
const glifistore::ManifestSegmentEntry kActive{.segment_id = glifistore::SegmentId{3},
                                               .generation = glifistore::GenerationId{1},
                                               .owner_worker = glifistore::WorkerId{0},
                                               .role = glifistore::ManifestSegmentRole::active};

// Two sealed Segments with mostly dead data and one active Segment, so that compaction of the
// sealed prefix reclaims a Segment.
auto seed_resident_store(const std::filesystem::path& path) -> glifistore::RecordRef {
    const auto store_id = recovery_store_id();
    auto directory = glifistore::DataDirectory::open_and_lock(path);
    GLIFI_REQUIRE(directory.has_value());
    auto first = create_segment(*directory, store_id, kSealedOne);
    append_record(first, 1, "stable", "stable-value");
    append_record(first, 2, "overwritten", "old");
    append_record(first, 3, "erased", "old");
    const auto records = first.scan_committed();
    GLIFI_REQUIRE(records.has_value());
    const auto stable_reference = records->front();
    GLIFI_REQUIRE(first.seal().committed());
    auto second = create_segment(*directory, store_id, kSealedTwo);
    append_record(second, 4, "overwritten", "new");
    append_record(second, 5, "erased", {}, glifistore::Opcode::erase);
    GLIFI_REQUIRE(second.seal().committed());
    auto active = create_segment(*directory, store_id, kActive);
    append_record(active, 6, "active", "active-value");
    GLIFI_REQUIRE(
        directory->publish_manifest(recovery_manifest(store_id, 1, {kSealedOne, kSealedTwo, kActive}))
            .durable());
    return stable_reference;
}

auto resident_put(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key,
                  const std::string_view value) -> glifistore::DurableMutationResult {
    return runtime.put(std::as_bytes(std::span{key}), std::as_bytes(std::span{value}));
}

void require_value(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key,
                   const std::string_view expected) {
    const auto value = runtime.get(key);
    GLIFI_REQUIRE(value.has_value());
    GLIFI_REQUIRE(owned_text(*value) == expected);
}

void require_absent(glifistore::DurableRuntimeCatalog& runtime, const std::string_view key) {
    const auto value = runtime.get(key);
    GLIFI_REQUIRE(!value.has_value());
    GLIFI_REQUIRE(value.error().code == glifistore::ErrorCode::not_found);
}

void overwrite_on_disk_byte(const std::filesystem::path& path, const glifistore::RecordRef& reference) {
    const auto identity = segment_identity(recovery_store_id(), kSealedOne);
    const auto segment_path = path / glifistore::segment_filename(identity);
    const auto descriptor = ::open(segment_path.c_str(), O_RDWR | O_CLOEXEC);
    GLIFI_REQUIRE(descriptor >= 0);
    const std::byte corruption{0xFF};
    const auto value_offset =
        static_cast<off_t>(reference.offset.value + glifistore::kEncodedRecordHeaderSize + 6U);
    GLIFI_REQUIRE(::pwrite(descriptor, &corruption, 1, value_offset) == 1);
    GLIFI_REQUIRE(::close(descriptor) == 0);
}

} // namespace

GLIFI_TEST("resident images serve durable reads across open, rotation and compaction") {
    RecoveryTemporaryDirectory temporary;
    static_cast<void>(seed_resident_store(temporary.path()));

    RotationBudgetObserver observer{.force_segment_full = false, .available_bytes = 1ULL << 40U};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.context = &observer,
                           .before = &RotationBudgetObserver::before,
                           .available_space_bytes = &RotationBudgetObserver::available});
    GLIFI_REQUIRE(directory.has_value());
    auto limits = glifistore::DurableResourceLimits{};
    limits.reserved_free_bytes = 0;
    auto runtime =
        glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, {.limits = limits});
    GLIFI_REQUIRE(runtime.has_value());
    auto& catalog = **runtime;

    const auto opened = catalog.memory_reserve_stats();
    GLIFI_REQUIRE(opened.index_charged_bytes > 0U);
    GLIFI_REQUIRE(opened.image_charged_bytes > 0U);
    require_value(catalog, "stable", "stable-value");
    require_value(catalog, "overwritten", "new");
    require_absent(catalog, "erased");
    require_value(catalog, "active", "active-value");

    // Appends are mirrored into the active image before publication.
    GLIFI_REQUIRE(resident_put(catalog, "fresh", "fresh-value").committed());
    require_value(catalog, "fresh", "fresh-value");

    // Rotation: the sealed Segment keeps the Writer's image and the new active one gets its own.
    observer.force_segment_full = true;
    GLIFI_REQUIRE(resident_put(catalog, "rotated", "rotated-value").committed());
    GLIFI_REQUIRE(!observer.force_segment_full);
    require_value(catalog, "fresh", "fresh-value");
    require_value(catalog, "rotated", "rotated-value");
    require_value(catalog, "active", "active-value");

    // Compaction outputs are born resident and replace the source images.
    const auto compacted = catalog.compact_worker(0, 0);
    GLIFI_REQUIRE(compacted.compacted());
    require_value(catalog, "overwritten", "new");
    require_absent(catalog, "erased");
    require_value(catalog, "fresh", "fresh-value");
    require_value(catalog, "rotated", "rotated-value");
    const auto after = catalog.memory_reserve_stats();
    GLIFI_REQUIRE(after.image_charged_bytes > 0U);
    GLIFI_REQUIRE(after.image_charged_bytes <= after.image_budget_bytes);
    GLIFI_REQUIRE(catalog.healthy());
}

GLIFI_TEST("zero memory reserve keeps every Segment cold without refusing the Index") {
    RecoveryTemporaryDirectory temporary;
    static_cast<void>(seed_resident_store(temporary.path()));
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    auto limits = glifistore::DurableResourceLimits{};
    limits.memory_reserve_bytes = 0;
    auto runtime =
        glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, {.limits = limits});
    GLIFI_REQUIRE(runtime.has_value());
    auto& catalog = **runtime;
    for (std::size_t key = 0; key < 512U; ++key) {
        GLIFI_REQUIRE(resident_put(catalog, "cold-" + std::to_string(key), "value").committed());
    }
    const auto stats = catalog.memory_reserve_stats();
    GLIFI_REQUIRE(stats.image_charged_bytes == 0U);
    GLIFI_REQUIRE(stats.index_charged_bytes > 0U);
    GLIFI_REQUIRE(stats.image_admission_failures > 0U);
    require_value(catalog, "stable", "stable-value");
    require_value(catalog, "cold-511", "value");
}

GLIFI_TEST("reopen reloads images and keeps reads exact with resident reads disabled") {
    RecoveryTemporaryDirectory temporary;
    static_cast<void>(seed_resident_store(temporary.path()));
    for (const bool resident : {true, false}) {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto limits = glifistore::DurableResourceLimits{};
        limits.resident_reads_enabled = resident;
        auto runtime =
            glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, {.limits = limits});
        GLIFI_REQUIRE(runtime.has_value());
        auto& catalog = **runtime;
        GLIFI_REQUIRE((catalog.memory_reserve_stats().image_charged_bytes > 0U) == resident);
        require_value(catalog, "stable", "stable-value");
        require_value(catalog, "overwritten", "new");
        require_absent(catalog, "erased");
        const auto key = std::string{"reopen-"} + (resident ? "resident" : "cold");
        GLIFI_REQUIRE(resident_put(catalog, key, "value").committed());
        require_value(catalog, key, "value");
        GLIFI_REQUIRE(catalog.close().has_value());
    }
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());
    require_value(**runtime, "reopen-resident", "value");
    require_value(**runtime, "reopen-cold", "value");
}

GLIFI_TEST("resident reads keep load-time bytes while compaction still detects on-disk damage") {
    RecoveryTemporaryDirectory temporary;
    const auto stable_reference = seed_resident_store(temporary.path());
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());
    auto& catalog = **runtime;
    require_value(catalog, "stable", "stable-value");

    // The image holds the bytes validated at open, so later media damage is not observed by a
    // resident read (HAZ-042). The cold-path test with images disabled asserts read detection.
    overwrite_on_disk_byte(temporary.path(), stable_reference);
    require_value(catalog, "stable", "stable-value");
    GLIFI_REQUIRE(catalog.healthy());

    // Containment: compaction re-reads and re-verifies every live source Record from the file.
    const auto compacted = catalog.compact_worker(0, 0);
    GLIFI_REQUIRE(!compacted.compacted());
    GLIFI_REQUIRE(compacted.error.has_value());
    GLIFI_REQUIRE(compacted.error->code == glifistore::ErrorCode::checksum_mismatch);
}
