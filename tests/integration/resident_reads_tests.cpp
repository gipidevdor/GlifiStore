#include "glifistore/persistence/runtime_catalog.hpp"
#include "glifistore/store/store.hpp"
#include "persistence_recovery_test_support.hpp"
#include "store/store_internal.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <thread>
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

// Forces the next Record write to report a full Segment. Unlike RotationBudgetObserver it may be
// armed by one thread while the Writer consumes it on another.
struct ConcurrentRotationObserver {
    std::atomic_bool force_segment_full{false};

    static auto before(void* opaque, const glifistore::FilesystemOperation operation) -> glifistore::Status {
        auto& observer = *static_cast<ConcurrentRotationObserver*>(opaque);
        if (operation == glifistore::FilesystemOperation::write_record &&
            observer.force_segment_full.exchange(false, std::memory_order_acq_rel)) {
            return glifistore::fail(glifistore::ErrorCode::segment_full,
                                    "injected full Segment before rotation");
        }
        return {};
    }

    static auto available(void*) -> glifistore::Result<std::uint64_t> {
        return std::uint64_t{1} << 40U;
    }
};

// A reserve that holds one active image and almost nothing else: every rotation must evict the
// Worker's sealed images before it can admit the next active one.
constexpr std::uint64_t kOneImageReserveBytes = glifistore::kSegmentSizeBytes + (std::uint64_t{1} << 20U);
constexpr std::size_t kEvictionRotations = 48;
constexpr std::size_t kEvictionReaders = 4;

[[nodiscard]] auto eviction_key(const std::size_t index) -> std::string {
    return "evict-" + std::to_string(index);
}

[[nodiscard]] auto eviction_value(const std::size_t index) -> std::string {
    return "value-" + std::to_string(index);
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

GLIFI_TEST("image eviction at rotation is safe against concurrent durable readers and compaction") {
    RecoveryTemporaryDirectory temporary;
    static_cast<void>(seed_resident_store(temporary.path()));
    ConcurrentRotationObserver observer;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.context = &observer,
                           .before = &ConcurrentRotationObserver::before,
                           .available_space_bytes = &ConcurrentRotationObserver::available});
    GLIFI_REQUIRE(directory.has_value());
    auto limits = glifistore::DurableResourceLimits{};
    limits.reserved_free_bytes = 0;
    limits.memory_reserve_bytes = kOneImageReserveBytes;
    auto runtime =
        glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, {.limits = limits});
    GLIFI_REQUIRE(runtime.has_value());
    auto& catalog = **runtime;

    // Readers race the Writer's rotations: each one reads a seeded key and every key published so
    // far, whose Segment may be resident, evicted or cold at that instant. Failures are counted,
    // not asserted, because the test macro must run on the test thread.
    std::atomic_size_t published{0};
    std::atomic_bool stop{false};
    std::atomic_size_t wrong_values{0};
    std::atomic_size_t reads{0};
    std::vector<std::thread> readers;
    readers.reserve(kEvictionReaders);
    for (std::size_t reader = 0; reader < kEvictionReaders; ++reader) {
        readers.emplace_back([&, reader] {
            std::size_t cursor = reader;
            while (!stop.load(std::memory_order_acquire)) {
                const auto stable = catalog.get("stable");
                if (!stable || owned_text(*stable) != "stable-value") {
                    wrong_values.fetch_add(1U, std::memory_order_relaxed);
                }
                const auto visible = published.load(std::memory_order_acquire);
                if (visible != 0U) {
                    const auto index = cursor++ % visible;
                    const auto value = catalog.get(eviction_key(index));
                    if (!value || owned_text(*value) != eviction_value(index)) {
                        wrong_values.fetch_add(1U, std::memory_order_relaxed);
                    }
                }
                reads.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }

    // Compactions race the rotations: their outputs borrow the sources' image bytes while a
    // rotation may evict those sources (ADR 0046 credit).
    std::atomic_size_t compactions{0};
    std::thread compactor{[&] {
        while (!stop.load(std::memory_order_acquire)) {
            if (catalog.compact_worker(0, 0).compacted()) {
                compactions.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }};

    // Compaction may temporarily reject mutations at its publication gate.
    // Retry only a definite pre-commit conflict so each published key exists.
    const auto put_after_conflict = [&](const std::string_view key, const std::string_view value) {
        const auto deadline = std::chrono::steady_clock::now() + kNativeConcurrencyDeadline;
        for (;;) {
            const auto result = resident_put(catalog, key, value);
            if (result.committed()) {
                return true;
            }
            if (result.outcome != glifistore::DurableMutationOutcome::not_committed || !result.error ||
                result.error->code != glifistore::ErrorCode::sequence_conflict ||
                std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::yield();
        }
    };
    std::size_t committed{};
    std::size_t churned{};
    for (std::size_t index = 0; index < kEvictionRotations; ++index) {
        observer.force_segment_full.store(true, std::memory_order_release);
        if (put_after_conflict(eviction_key(index), eviction_value(index))) {
            ++committed;
        }
        if (committed != index + 1U) {
            break;
        }
        published.store(index + 1U, std::memory_order_release);
        // A superseded Record next to each live one makes every sealed Segment worth compacting
        // into an output that keeps the live Record.
        if (put_after_conflict("churn", eviction_value(index))) {
            ++churned;
        }
    }
    // Keep the readers on the final layout for a moment before stopping them.
    const auto reads_at_end = reads.load(std::memory_order_relaxed);
    while (reads.load(std::memory_order_relaxed) < reads_at_end + kEvictionReaders * 8U) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    compactor.join();
    for (auto& reader : readers) {
        reader.join();
    }

    GLIFI_REQUIRE(committed == kEvictionRotations);
    GLIFI_REQUIRE(churned == kEvictionRotations);
    GLIFI_REQUIRE(wrong_values.load() == 0U);
    GLIFI_REQUIRE(catalog.healthy());
    GLIFI_REQUIRE(compactions.load() > 0U);
    const auto stats = catalog.memory_reserve_stats();
    GLIFI_REQUIRE(stats.image_evictions > 0U);
    // Outputs never borrow bytes an eviction already returned, so images fit the budget.
    GLIFI_REQUIRE(stats.image_charged_bytes <= stats.image_budget_bytes);
    for (std::size_t index = 0; index < kEvictionRotations; ++index) {
        require_value(catalog, eviction_key(index), eviction_value(index));
    }
}

GLIFI_TEST("paired Readers stay exact while rotations evict resident images") {
    RecoveryTemporaryDirectory temporary;
    ConcurrentRotationObserver observer;
    auto limits = glifistore::DurableResourceLimits{};
    limits.reserved_free_bytes = 0;
    limits.memory_reserve_bytes = kOneImageReserveBytes;
    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::paired,
         .storage_mode = glifistore::StorageMode::durable_sync,
         .data_directory = temporary.path() / "paired-store",
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_limits = limits,
         .filesystem_hooks = {.context = &observer,
                              .before = &ConcurrentRotationObserver::before,
                              .available_space_bytes = &ConcurrentRotationObserver::available}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;

    std::atomic_size_t published{0};
    std::atomic_bool stop{false};
    std::atomic_size_t wrong_values{0};
    std::vector<std::thread> readers;
    readers.reserve(kEvictionReaders);
    for (std::size_t reader = 0; reader < kEvictionReaders; ++reader) {
        readers.emplace_back([&, reader] {
            std::size_t cursor = reader;
            while (!stop.load(std::memory_order_acquire)) {
                const auto visible = published.load(std::memory_order_acquire);
                if (visible == 0U) {
                    std::this_thread::yield();
                    continue;
                }
                const auto index = cursor++ % visible;
                const auto value = store.get(eviction_key(index));
                if (!value || owned_text(*value) != eviction_value(index)) {
                    wrong_values.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }

    std::size_t committed{};
    for (std::size_t index = 0; index < kEvictionRotations; ++index) {
        observer.force_segment_full.store(true, std::memory_order_release);
        const auto key = eviction_key(index);
        const auto value = eviction_value(index);
        if (store.put(key, std::as_bytes(std::span{value})).has_value()) {
            ++committed;
        }
        published.store(index + 1U, std::memory_order_release);
    }
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }

    GLIFI_REQUIRE(committed == kEvictionRotations);
    GLIFI_REQUIRE(wrong_values.load() == 0U);
    const auto reserve = glifistore::detail::StoreAccess::memory_reserve(store);
    GLIFI_REQUIRE(reserve != nullptr);
    const auto stats = reserve->stats();
    GLIFI_REQUIRE(stats.image_evictions > 0U);
    // The Worker Index and the paired Reader bases are both charged.
    GLIFI_REQUIRE(stats.index_charged_bytes > 0U);
    for (std::size_t index = 0; index < kEvictionRotations; ++index) {
        const auto value = store.get(eviction_key(index));
        GLIFI_REQUIRE(value.has_value());
        GLIFI_REQUIRE(owned_text(*value) == eviction_value(index));
    }
    GLIFI_REQUIRE(store.close().has_value());
}
