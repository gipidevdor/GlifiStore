#include "persistence_recovery_test_support.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Blocked durable I/O must not hold Worker/catalog mutexes (split for structure budget).

GLIFI_TEST("blocked durable append holds no Worker or catalog mutex") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const glifistore::ManifestSegmentEntry active{
        .segment_id = glifistore::SegmentId{1},
        .generation = glifistore::GenerationId{1},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::active,
    };
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_segment(*directory, store_id, active));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
    }

    BlockingFilesystemOperation blocker{glifistore::FilesystemOperation::sync_record, false};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.context = &blocker, .before = &BlockingFilesystemOperation::before});
    GLIFI_REQUIRE(directory.has_value());
    glifistore::DurableRuntimeOptions options;
    options.limits.hot_cache_enabled = false;
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, options);
    GLIFI_REQUIRE(runtime.has_value());
    const std::string old_key{"append-readable"};
    const std::string new_key{"append-writer"};
    const std::string value{"value"};
    GLIFI_REQUIRE(
        (*runtime)->put(std::as_bytes(std::span{old_key}), std::as_bytes(std::span{value})).committed());

    blocker.arm();
    glifistore::DurableMutationResult appended;
    std::thread writer{[&] {
        appended = (*runtime)->put(std::as_bytes(std::span{new_key}), std::as_bytes(std::span{value}));
    }};
    GLIFI_REQUIRE(blocker.wait_until_blocked());

    const auto visible = (*runtime)->get(old_key);
    blocker.release();
    writer.join();
    GLIFI_REQUIRE(visible.has_value());
    GLIFI_REQUIRE(owned_text(*visible) == value);
    GLIFI_REQUIRE(appended.committed());
    GLIFI_REQUIRE((*runtime)->get(new_key).has_value());
}

GLIFI_TEST("blocked group commit holds no Worker or catalog mutex") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const glifistore::ManifestSegmentEntry active{
        .segment_id = glifistore::SegmentId{1},
        .generation = glifistore::GenerationId{1},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::active,
    };
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_segment(*directory, store_id, active));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
    }

    BlockingFilesystemOperation blocker{glifistore::FilesystemOperation::sync_record, false};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.context = &blocker, .before = &BlockingFilesystemOperation::before});
    GLIFI_REQUIRE(directory.has_value());
    glifistore::DurableRuntimeOptions options;
    options.limits.hot_cache_enabled = false;
    options.batch =
        glifistore::DurableGroupConfig{.max_records = 1, .max_bytes = 1U << 20U, .max_wait_ms = 1000};
    options.strict_ack = true;
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, options);
    GLIFI_REQUIRE(runtime.has_value());
    const std::string old_key{"batch-readable"};
    const std::string new_key{"batch-writer"};
    const std::string value{"value"};
    GLIFI_REQUIRE(
        (*runtime)->put(std::as_bytes(std::span{old_key}), std::as_bytes(std::span{value})).committed());

    blocker.arm();
    glifistore::DurableMutationResult committed;
    std::thread writer{[&] {
        committed = (*runtime)->put(std::as_bytes(std::span{new_key}), std::as_bytes(std::span{value}));
    }};
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    const auto visible = (*runtime)->get(old_key);
    blocker.release();
    writer.join();
    GLIFI_REQUIRE(visible.has_value());
    GLIFI_REQUIRE(owned_text(*visible) == value);
    GLIFI_REQUIRE(committed.committed());
}

GLIFI_TEST("blocked deferred dirty sync holds no Worker or catalog mutex") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const glifistore::ManifestSegmentEntry active{
        .segment_id = glifistore::SegmentId{1},
        .generation = glifistore::GenerationId{1},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::active,
    };
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_segment(*directory, store_id, active));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
    }

    BlockingFilesystemOperation blocker{glifistore::FilesystemOperation::sync_commit_slot, false};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.context = &blocker, .before = &BlockingFilesystemOperation::before});
    GLIFI_REQUIRE(directory.has_value());
    glifistore::DurableRuntimeOptions options;
    options.limits.hot_cache_enabled = false;
    options.commit_sync = glifistore::SegmentCommitSync::deferred;
    options.sync_interval_ms = 60'000;
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, options);
    GLIFI_REQUIRE(runtime.has_value());
    const std::string key{"dirty-sync-readable"};
    const std::string value{"value"};
    GLIFI_REQUIRE(
        (*runtime)->put(std::as_bytes(std::span{key}), std::as_bytes(std::span{value})).committed());

    blocker.arm();
    std::optional<glifistore::Status> flushed;
    std::thread flusher{[&] { flushed.emplace((*runtime)->flush()); }};
    GLIFI_REQUIRE(blocker.wait_until_blocked());
    const auto visible = (*runtime)->get(key);
    blocker.release();
    flusher.join();
    GLIFI_REQUIRE(visible.has_value());
    GLIFI_REQUIRE(owned_text(*visible) == value);
    GLIFI_REQUIRE(flushed.has_value());
    GLIFI_REQUIRE(flushed->has_value());
}

GLIFI_TEST("blocked rotation seal holds no Worker or catalog mutex") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const glifistore::ManifestSegmentEntry active{
        .segment_id = glifistore::SegmentId{1},
        .generation = glifistore::GenerationId{1},
        .owner_worker = glifistore::WorkerId{0},
        .role = glifistore::ManifestSegmentRole::active,
    };
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        static_cast<void>(create_segment(*directory, store_id, active));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
    }

    BlockingRotationSeal blocker;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.context = &blocker, .before = &BlockingRotationSeal::before});
    GLIFI_REQUIRE(directory.has_value());
    glifistore::DurableRuntimeOptions options;
    options.limits.hot_cache_enabled = false;
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, options);
    GLIFI_REQUIRE(runtime.has_value());
    const std::string old_key{"rotation-readable"};
    const std::string new_key{"rotation-writer"};
    const std::string value{"value"};
    GLIFI_REQUIRE(
        (*runtime)->put(std::as_bytes(std::span{old_key}), std::as_bytes(std::span{value})).committed());

    blocker.arm();
    glifistore::DurableMutationResult rotated;
    std::thread writer{[&] {
        rotated = (*runtime)->put(std::as_bytes(std::span{new_key}), std::as_bytes(std::span{value}));
    }};
    GLIFI_REQUIRE(blocker.wait_until_blocked());

    std::mutex completion_mutex;
    std::condition_variable completion;
    bool read_finished{};
    std::optional<glifistore::Result<glifistore::OwnedValue>> read;
    std::thread reader{[&] {
        read.emplace((*runtime)->get(old_key));
        {
            const std::lock_guard lock{completion_mutex};
            read_finished = true;
        }
        completion.notify_one();
    }};
    bool completed_while_seal_blocked{};
    {
        std::unique_lock lock{completion_mutex};
        completed_while_seal_blocked =
            completion.wait_for(lock, std::chrono::seconds{1}, [&] { return read_finished; });
    }

    blocker.release();
    reader.join();
    writer.join();
    GLIFI_REQUIRE(completed_while_seal_blocked);
    GLIFI_REQUIRE(read.has_value());
    GLIFI_REQUIRE(read->has_value());
    GLIFI_REQUIRE(owned_text(**read) == value);
    GLIFI_REQUIRE(rotated.committed());
    GLIFI_REQUIRE((*runtime)->get(new_key).has_value());
    GLIFI_REQUIRE((*runtime)->manifest().segments.size() == 2);
}
