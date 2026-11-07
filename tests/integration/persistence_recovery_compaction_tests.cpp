#include "persistence_recovery_test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

GLIFI_TEST("blocked durable compaction build permits same-Worker reads and mutations") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto first = create_segment(*directory, store_id, entries[0]);
        append_record(first, 1, "changing", "old");
        GLIFI_REQUIRE(first.seal().committed());
        auto second = create_segment(*directory, store_id, entries[1]);
        append_record(second, 2, "stable", "visible");
        append_record(second, 3, "erase-me", "present");
        append_record(second, 4, "ttl-key", "old-ttl");
        GLIFI_REQUIRE(second.seal().committed());
        static_cast<void>(create_segment(*directory, store_id, entries[2]));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
    }

    BlockingRecordRead blocked_build;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(),
        glifistore::FilesystemHooks{
            .context = &blocked_build,
            .file_io = {.context = &blocked_build, .read_some_at = &BlockingRecordRead::read_some_at}});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());
    blocked_build.arm();

    glifistore::DurableCompactionResult compaction;
    std::thread compactor{[&] { compaction = (*runtime)->compact_worker(0, 0); }};
    const bool build_blocked = blocked_build.wait_until_blocked();
    if (!build_blocked) {
        blocked_build.release();
        compactor.join();
    }
    GLIFI_REQUIRE(build_blocked);

    const std::string key{"changing"};
    const std::string replacement{"new"};
    std::optional<glifistore::Result<glifistore::OwnedValue>> stable;
    glifistore::DurableMutationResult mutation;
    glifistore::DurableMutationResult erased;
    glifistore::DurableMutationResult ttl_updated;
    std::mutex completion_mutex;
    std::condition_variable completion;
    bool operations_finished{};
    std::thread operations{[&] {
        stable.emplace((*runtime)->get("stable"));
        mutation = (*runtime)->put(std::as_bytes(std::span{key}), std::as_bytes(std::span{replacement}));
        const std::string erased_key{"erase-me"};
        erased = (*runtime)->erase(std::as_bytes(std::span{erased_key}));
        const std::string ttl_key{"ttl-key"};
        const std::string ttl_value{"new-ttl"};
        ttl_updated =
            (*runtime)->put(std::as_bytes(std::span{ttl_key}), std::as_bytes(std::span{ttl_value}), 100);
        {
            const std::lock_guard lock{completion_mutex};
            operations_finished = true;
        }
        completion.notify_one();
    }};
    bool completed_during_build{};
    {
        std::unique_lock lock{completion_mutex};
        completed_during_build =
            completion.wait_for(lock, kNativeConcurrencyDeadline, [&] { return operations_finished; });
    }

    blocked_build.release();
    operations.join();
    compactor.join();

    GLIFI_REQUIRE(completed_during_build);
    GLIFI_REQUIRE(stable.has_value());
    GLIFI_REQUIRE(stable->has_value());
    GLIFI_REQUIRE(owned_text(**stable) == "visible");
    GLIFI_REQUIRE(mutation.committed());
    GLIFI_REQUIRE(erased.committed());
    GLIFI_REQUIRE(ttl_updated.committed());
    GLIFI_REQUIRE(compaction.compacted());
    GLIFI_REQUIRE(!compaction.error.has_value());
    GLIFI_REQUIRE(compaction.stats.delta_records == 3);
    GLIFI_REQUIRE((*runtime)->healthy());
    const auto current = (*runtime)->get(key);
    GLIFI_REQUIRE(current.has_value());
    GLIFI_REQUIRE(owned_text(*current) == replacement);
    const auto erased_value = (*runtime)->get("erase-me");
    GLIFI_REQUIRE(!erased_value.has_value());
    GLIFI_REQUIRE(erased_value.error().code == glifistore::ErrorCode::not_found);
    const auto ttl_visible = (*runtime)->get("ttl-key", 99);
    GLIFI_REQUIRE(ttl_visible.has_value());
    GLIFI_REQUIRE(owned_text(*ttl_visible) == "new-ttl");
    const auto ttl_expired = (*runtime)->get("ttl-key", 100);
    GLIFI_REQUIRE(!ttl_expired.has_value());
    GLIFI_REQUIRE(ttl_expired.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*runtime)->manifest().manifest_generation == 2);
    GLIFI_REQUIRE((*runtime)->namespace_audit().clean());
}

GLIFI_TEST("blocked pre-intent compaction copy lets an unrelated rotation commit") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{4},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{1},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    const auto compacted_key = key_for_worker(0, 2, "compact-");
    const auto second_compacted_key = key_for_worker(0, 2, "compact-second-");
    const auto rotating_key = key_for_worker(1, 2, "rotate-");
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto first = create_segment(*directory, store_id, entries[0]);
        append_record(first, 1, compacted_key, "first");
        GLIFI_REQUIRE(first.seal().committed());
        auto second = create_segment(*directory, store_id, entries[1]);
        append_record(second, 2, second_compacted_key, "second");
        GLIFI_REQUIRE(second.seal().committed());
        static_cast<void>(create_segment(*directory, store_id, entries[2]));
        static_cast<void>(create_segment(*directory, store_id, entries[3]));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 2, entries)).durable());
    }

    BlockingFilesystemOperation blocked_copy{glifistore::FilesystemOperation::write_record};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), glifistore::FilesystemHooks{.context = &blocked_copy,
                                                      .before = &BlockingFilesystemOperation::before});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());

    glifistore::DurableCompactionResult compaction;
    std::thread compactor{[&] { compaction = (*runtime)->compact_worker(0, 0); }};
    const bool copy_blocked = blocked_copy.wait_until_blocked();
    if (!copy_blocked) {
        // Do not unwind through a joinable thread: release a late hook and
        // preserve the actual failed requirement for the test runner.
        blocked_copy.release();
        compactor.join();
    }
    GLIFI_REQUIRE(copy_blocked);
    blocked_copy.force_next_record_write_full();

    glifistore::DurableMutationResult rotation;
    std::mutex completion_mutex;
    std::condition_variable completion;
    bool rotation_finished{};
    std::thread writer{[&] {
        const std::string value{"value"};
        rotation = (*runtime)->put(std::as_bytes(std::span{rotating_key}), std::as_bytes(std::span{value}));
        {
            const std::lock_guard lock{completion_mutex};
            rotation_finished = true;
        }
        completion.notify_one();
    }};
    bool rotation_completed_during_copy{};
    {
        std::unique_lock lock{completion_mutex};
        rotation_completed_during_copy =
            completion.wait_for(lock, kNativeConcurrencyDeadline, [&] { return rotation_finished; });
    }

    // A completed put has already published rotation telemetry. Snapshot it
    // before releasing compaction, then clean up both threads before asserting
    // so a timeout is reported as a normal test failure rather than terminate.
    const auto in_flight_rotation_stats = (*runtime)->rotation_stats();
    blocked_copy.release();
    writer.join();
    compactor.join();

    GLIFI_REQUIRE(rotation_completed_during_copy);
    GLIFI_REQUIRE(in_flight_rotation_stats.attempts == 1);
    GLIFI_REQUIRE(in_flight_rotation_stats.committed == 1);
    GLIFI_REQUIRE(in_flight_rotation_stats.compaction_waits == 0);
    GLIFI_REQUIRE(in_flight_rotation_stats.final_record_commit_attempts == 1);
    GLIFI_REQUIRE(in_flight_rotation_stats.last_total_duration_ns > 0);
    GLIFI_REQUIRE(rotation.committed());
    GLIFI_REQUIRE(compaction.outcome == glifistore::DurableCompactionOutcome::not_compacted);
    GLIFI_REQUIRE(compaction.error.has_value());
    GLIFI_REQUIRE(compaction.error->code == glifistore::ErrorCode::sequence_conflict);
    const auto rotation_stats = (*runtime)->rotation_stats();
    GLIFI_REQUIRE(rotation_stats.attempts == 1);
    GLIFI_REQUIRE(rotation_stats.committed == 1);
    GLIFI_REQUIRE(rotation_stats.compaction_waits == 0);
    GLIFI_REQUIRE(rotation_stats.final_record_commit_attempts == 1);
    GLIFI_REQUIRE(rotation_stats.final_record_commits == 1);
    GLIFI_REQUIRE(rotation_stats.last_seal_duration_ns > 0);
    GLIFI_REQUIRE(rotation_stats.last_create_duration_ns > 0);
    GLIFI_REQUIRE(rotation_stats.last_manifest_publication_duration_ns > 0);
    GLIFI_REQUIRE(rotation_stats.last_execution_duration_ns > 0);
    GLIFI_REQUIRE(rotation_stats.last_execution_duration_ns >=
                  rotation_stats.last_seal_duration_ns + rotation_stats.last_create_duration_ns +
                      rotation_stats.last_manifest_publication_duration_ns);
    GLIFI_REQUIRE(rotation_stats.last_total_duration_ns >= rotation_stats.last_publication_wait_duration_ns);
    GLIFI_REQUIRE(rotation_stats.last_total_duration_ns >= rotation_stats.last_execution_duration_ns);
    GLIFI_REQUIRE(rotation_stats.total_duration_ns == rotation_stats.last_total_duration_ns);
    GLIFI_REQUIRE(rotation_stats.maximum_total_duration_ns == rotation_stats.last_total_duration_ns);
    GLIFI_REQUIRE(rotation_stats.last_final_record_commit_duration_ns > 0);
    GLIFI_REQUIRE(rotation_stats.total_final_record_commit_duration_ns ==
                  rotation_stats.last_final_record_commit_duration_ns);
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE((*runtime)->manifest().segments.size() == 5);
    GLIFI_REQUIRE((*runtime)->namespace_audit().clean());
    const auto visible = (*runtime)->get(rotating_key);
    GLIFI_REQUIRE(visible.has_value());
    GLIFI_REQUIRE(owned_text(*visible) == "value");
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    const auto durable = (*reopened)->get(rotating_key);
    GLIFI_REQUIRE(durable.has_value());
    GLIFI_REQUIRE(owned_text(*durable) == "value");
    GLIFI_REQUIRE((*reopened)->manifest().segments.size() == 5);
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
}

GLIFI_TEST("rotation waiting on compaction intent does not block its Worker queue") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{4},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{1},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    const auto first_key = key_for_worker(1, 2, "waiting-rotation-");
    const auto queued_key = key_for_worker(1, 2, "queue-progress-");
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto first = create_segment(*directory, store_id, entries[0]);
        append_record(first, 1, key_for_worker(0, 2, "compact-a-"), "first");
        GLIFI_REQUIRE(first.seal().committed());
        auto second = create_segment(*directory, store_id, entries[1]);
        append_record(second, 2, key_for_worker(0, 2, "compact-b-"), "second");
        GLIFI_REQUIRE(second.seal().committed());
        static_cast<void>(create_segment(*directory, store_id, entries[2]));
        static_cast<void>(create_segment(*directory, store_id, entries[3]));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 2, entries)).durable());
    }

    BlockingFilesystemOperation blocker{glifistore::FilesystemOperation::write_compaction_intent};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), {.context = &blocker, .before = &BlockingFilesystemOperation::before});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());

    glifistore::DurableCompactionResult compaction;
    std::thread compactor{[&] { compaction = (*runtime)->compact_worker(0, 0); }};
    const bool intent_blocked = blocker.wait_until_blocked();
    if (!intent_blocked) {
        blocker.release();
        compactor.join();
    }
    GLIFI_REQUIRE(intent_blocked);

    blocker.force_next_record_write_full();
    glifistore::DurableMutationResult rotating;
    std::thread rotation{[&] {
        const std::string value{"rotation"};
        rotating = (*runtime)->put(std::as_bytes(std::span{first_key}), std::as_bytes(std::span{value}));
    }};
    auto rotation_stats = (*runtime)->rotation_stats();
    const auto rotation_deadline = std::chrono::steady_clock::now() + kNativeConcurrencyDeadline;
    while (rotation_stats.attempts == 0 && std::chrono::steady_clock::now() < rotation_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        rotation_stats = (*runtime)->rotation_stats();
    }

    glifistore::DurableMutationResult queued;
    std::mutex completion_mutex;
    std::condition_variable completion;
    bool queued_finished{};
    std::thread queued_writer{[&] {
        const std::string value{"queued"};
        queued = (*runtime)->put(std::as_bytes(std::span{queued_key}), std::as_bytes(std::span{value}));
        {
            const std::lock_guard lock{completion_mutex};
            queued_finished = true;
        }
        completion.notify_one();
    }};
    bool queue_progressed{};
    {
        std::unique_lock lock{completion_mutex};
        queue_progressed =
            completion.wait_for(lock, kNativeConcurrencyDeadline, [&] { return queued_finished; });
    }

    blocker.release();
    queued_writer.join();
    rotation.join();
    compactor.join();

    GLIFI_REQUIRE(rotation_stats.attempts == 1);
    GLIFI_REQUIRE(queue_progressed);
    GLIFI_REQUIRE(queued.committed());
    GLIFI_REQUIRE(!rotating.committed());
    GLIFI_REQUIRE(rotating.error.has_value());
    GLIFI_REQUIRE(rotating.error->code == glifistore::ErrorCode::sequence_conflict);
    GLIFI_REQUIRE(compaction.compacted());
    GLIFI_REQUIRE((*runtime)->healthy());
    const auto rejected = (*runtime)->get(first_key);
    GLIFI_REQUIRE(!rejected.has_value());
    GLIFI_REQUIRE(rejected.error().code == glifistore::ErrorCode::not_found);
    GLIFI_REQUIRE((*runtime)->get(queued_key).has_value());
}

GLIFI_TEST("compaction manifest sync holds no Worker or catalog mutex") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{4},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{1},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    const auto stable_key = key_for_worker(0, 2, "stable-");
    const auto second_key = key_for_worker(0, 2, "second-");
    const auto rejected_key = key_for_worker(0, 2, "rejected-");
    const auto other_worker_key = key_for_worker(1, 2, "other-");
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto first = create_segment(*directory, store_id, entries[0]);
        append_record(first, 1, stable_key, "stable-value");
        GLIFI_REQUIRE(first.seal().committed());
        auto second = create_segment(*directory, store_id, entries[1]);
        append_record(second, 2, second_key, "second-value");
        GLIFI_REQUIRE(second.seal().committed());
        static_cast<void>(create_segment(*directory, store_id, entries[2]));
        static_cast<void>(create_segment(*directory, store_id, entries[3]));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 2, entries)).durable());
    }

    BlockingFilesystemOperation blocked_sync{glifistore::FilesystemOperation::sync_manifest};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(), glifistore::FilesystemHooks{.context = &blocked_sync,
                                                      .before = &BlockingFilesystemOperation::before});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());

    glifistore::DurableCompactionResult compaction;
    std::thread compactor{[&] { compaction = (*runtime)->compact_worker(0, 0); }};
    GLIFI_REQUIRE(blocked_sync.wait_until_blocked());

    std::optional<glifistore::Result<glifistore::OwnedValue>> read;
    glifistore::DurableMutationResult rejected;
    glifistore::DurableMutationResult other_worker;
    std::mutex completion_mutex;
    std::condition_variable completion;
    bool operations_finished{};
    std::thread operations{[&] {
        read.emplace((*runtime)->get(stable_key));
        const std::string rejected_value{"rejected"};
        rejected =
            (*runtime)->put(std::as_bytes(std::span{rejected_key}), std::as_bytes(std::span{rejected_value}));
        const std::string other_value{"other-value"};
        other_worker = (*runtime)->put(std::as_bytes(std::span{other_worker_key}),
                                       std::as_bytes(std::span{other_value}));
        {
            const std::lock_guard lock{completion_mutex};
            operations_finished = true;
        }
        completion.notify_one();
    }};
    bool completed_during_manifest_sync{};
    {
        std::unique_lock lock{completion_mutex};
        completed_during_manifest_sync =
            completion.wait_for(lock, kNativeConcurrencyDeadline, [&] { return operations_finished; });
    }

    blocked_sync.release();
    operations.join();
    compactor.join();

    GLIFI_REQUIRE(completed_during_manifest_sync);
    GLIFI_REQUIRE(read.has_value());
    GLIFI_REQUIRE(read->has_value());
    GLIFI_REQUIRE(owned_text(**read) == "stable-value");
    GLIFI_REQUIRE(rejected.outcome == glifistore::DurableMutationOutcome::not_committed);
    GLIFI_REQUIRE(rejected.error.has_value());
    GLIFI_REQUIRE(rejected.error->code == glifistore::ErrorCode::sequence_conflict);
    GLIFI_REQUIRE(other_worker.committed());
    GLIFI_REQUIRE(compaction.compacted());
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE((*runtime)->namespace_audit().clean());
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    const auto durable_other = (*reopened)->get(other_worker_key);
    GLIFI_REQUIRE(durable_other.has_value());
    GLIFI_REQUIRE(owned_text(*durable_other) == "other-value");
}

GLIFI_TEST("close during a blocked compaction build rolls back the old authority") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto first = create_segment(*directory, store_id, entries[0]);
        append_record(first, 1, "first", "value");
        GLIFI_REQUIRE(first.seal().committed());
        auto second = create_segment(*directory, store_id, entries[1]);
        append_record(second, 2, "second", "value");
        GLIFI_REQUIRE(second.seal().committed());
        static_cast<void>(create_segment(*directory, store_id, entries[2]));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
    }

    BlockingRecordRead blocked_build;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(),
        glifistore::FilesystemHooks{
            .context = &blocked_build,
            .file_io = {.context = &blocked_build, .read_some_at = &BlockingRecordRead::read_some_at}});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());
    blocked_build.arm();

    glifistore::DurableCompactionResult compaction;
    std::thread compactor{[&] { compaction = (*runtime)->compact_worker(0, 0); }};
    GLIFI_REQUIRE(blocked_build.wait_until_blocked());
    const auto closed = (*runtime)->close();
    GLIFI_REQUIRE(closed.has_value());

    blocked_build.release();
    compactor.join();
    GLIFI_REQUIRE(compaction.outcome == glifistore::DurableCompactionOutcome::not_compacted);
    GLIFI_REQUIRE(compaction.error.has_value());
    GLIFI_REQUIRE(compaction.error->code == glifistore::ErrorCode::sequence_conflict);
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->manifest() == recovery_manifest(store_id, 1, entries));
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    GLIFI_REQUIRE((*reopened)->get("first").has_value());
    GLIFI_REQUIRE((*reopened)->get("second").has_value());
}

GLIFI_TEST("online compaction filesystem fault matrix reopens one clean authority") {
    struct FaultCase {
        glifistore::FilesystemOperation operation;
        std::size_t occurrence{1};
    };

    const std::vector<FaultCase> faults{
        {glifistore::FilesystemOperation::write_compaction_intent},
        {glifistore::FilesystemOperation::sync_compaction_intent},
        {glifistore::FilesystemOperation::rename_compaction_intent},
        {glifistore::FilesystemOperation::sync_directory, 1},
        {glifistore::FilesystemOperation::preallocate_segment},
        {glifistore::FilesystemOperation::write_segment_header},
        {glifistore::FilesystemOperation::rename_segment},
        {glifistore::FilesystemOperation::sync_directory, 2},
        {glifistore::FilesystemOperation::write_record, 1},
        {glifistore::FilesystemOperation::write_record, 2},
        {glifistore::FilesystemOperation::sync_record},
        {glifistore::FilesystemOperation::write_commit_slot, 1},
        {glifistore::FilesystemOperation::sync_commit_slot, 1},
        {glifistore::FilesystemOperation::write_commit_slot, 2},
        {glifistore::FilesystemOperation::sync_commit_slot, 2},
        {glifistore::FilesystemOperation::write_manifest},
        {glifistore::FilesystemOperation::sync_manifest},
        {glifistore::FilesystemOperation::rename_manifest},
        {glifistore::FilesystemOperation::sync_directory, 3},
        {glifistore::FilesystemOperation::remove_compaction_segment, 1},
        {glifistore::FilesystemOperation::remove_compaction_segment, 2},
        {glifistore::FilesystemOperation::sync_directory, 4},
        {glifistore::FilesystemOperation::remove_compaction_intent},
        {glifistore::FilesystemOperation::sync_directory, 5},
    };
    for (const auto& fault : faults) {
        RecoveryTemporaryDirectory temporary;
        const auto store_id = recovery_store_id();
        const std::vector entries{
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::sealed},
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::sealed},
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::active},
        };
        {
            auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
            GLIFI_REQUIRE(directory.has_value());
            auto first = create_segment(*directory, store_id, entries[0]);
            append_record(first, 1, "fault-first", "first-value");
            GLIFI_REQUIRE(first.seal().committed());
            auto second = create_segment(*directory, store_id, entries[1]);
            append_record(second, 2, "fault-second", "second-value");
            GLIFI_REQUIRE(second.seal().committed());
            static_cast<void>(create_segment(*directory, store_id, entries[2]));
            GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
        }

        OneShotFilesystemFailure failure{.target = fault.operation, .target_occurrence = fault.occurrence};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(),
            glifistore::FilesystemHooks{.context = &failure, .before = &OneShotFilesystemFailure::before});
        GLIFI_REQUIRE(directory.has_value());
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
        GLIFI_REQUIRE(runtime.has_value());
        const auto result = (*runtime)->compact_worker(0, 0);
        GLIFI_REQUIRE(failure.fired);
        GLIFI_REQUIRE(!result.compacted());
        GLIFI_REQUIRE(result.error.has_value());
        GLIFI_REQUIRE(result.error->code == glifistore::ErrorCode::io_error);
        GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::not_compacted ||
                      result.outcome == glifistore::DurableCompactionOutcome::recovery_required);
        GLIFI_REQUIRE((*runtime)->healthy() ==
                      (result.outcome == glifistore::DurableCompactionOutcome::not_compacted));
        runtime->reset();

        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        const auto first = (*reopened)->get("fault-first");
        const auto second = (*reopened)->get("fault-second");
        GLIFI_REQUIRE(first.has_value());
        GLIFI_REQUIRE(second.has_value());
        GLIFI_REQUIRE(owned_text(*first) == "first-value");
        GLIFI_REQUIRE(owned_text(*second) == "second-value");
    }
}

// GS-PERSIST-FAULT-001 / Wave 3 L4: capacity errno class at compaction publication
// boundaries (storage_exhausted), distinct from the generic io_error matrix above.
// E0–E2 fault-injection only; not E3/E4 physical certification.
GLIFI_TEST("online compaction storage_exhausted fault matrix reopens one clean authority") {
    struct FaultCase {
        glifistore::FilesystemOperation operation;
        std::size_t occurrence{1};
    };

    const std::vector<FaultCase> faults{
        {glifistore::FilesystemOperation::preallocate_segment},
        {glifistore::FilesystemOperation::write_segment_header},
        {glifistore::FilesystemOperation::write_record, 1},
        {glifistore::FilesystemOperation::sync_record},
        {glifistore::FilesystemOperation::write_commit_slot, 1},
        {glifistore::FilesystemOperation::sync_commit_slot, 1},
        {glifistore::FilesystemOperation::write_compaction_intent},
        {glifistore::FilesystemOperation::sync_compaction_intent},
        {glifistore::FilesystemOperation::rename_compaction_intent},
        {glifistore::FilesystemOperation::sync_directory, 1},
        {glifistore::FilesystemOperation::rename_segment},
        {glifistore::FilesystemOperation::write_manifest},
        {glifistore::FilesystemOperation::sync_manifest},
        {glifistore::FilesystemOperation::rename_manifest},
        {glifistore::FilesystemOperation::remove_compaction_segment, 1},
        {glifistore::FilesystemOperation::remove_compaction_intent},
    };
    for (const auto& fault : faults) {
        RecoveryTemporaryDirectory temporary;
        const auto store_id = recovery_store_id();
        const std::vector entries{
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::sealed},
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::sealed},
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::active},
        };
        {
            auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
            GLIFI_REQUIRE(directory.has_value());
            auto first = create_segment(*directory, store_id, entries[0]);
            append_record(first, 1, "enospc-first", "first-value");
            GLIFI_REQUIRE(first.seal().committed());
            auto second = create_segment(*directory, store_id, entries[1]);
            append_record(second, 2, "enospc-second", "second-value");
            GLIFI_REQUIRE(second.seal().committed());
            static_cast<void>(create_segment(*directory, store_id, entries[2]));
            GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
        }

        OneShotFilesystemFailure failure{.target = fault.operation,
                                         .code = glifistore::ErrorCode::storage_exhausted,
                                         .target_occurrence = fault.occurrence};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(),
            glifistore::FilesystemHooks{.context = &failure, .before = &OneShotFilesystemFailure::before});
        GLIFI_REQUIRE(directory.has_value());
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
        GLIFI_REQUIRE(runtime.has_value());
        const auto result = (*runtime)->compact_worker(0, 0);
        GLIFI_REQUIRE(failure.fired);
        GLIFI_REQUIRE(!result.compacted());
        GLIFI_REQUIRE(result.error.has_value());
        GLIFI_REQUIRE(result.error->code == glifistore::ErrorCode::storage_exhausted);
        GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::not_compacted ||
                      result.outcome == glifistore::DurableCompactionOutcome::recovery_required);
        GLIFI_REQUIRE((*runtime)->healthy() ==
                      (result.outcome == glifistore::DurableCompactionOutcome::not_compacted));
        runtime->reset();

        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        const auto first = (*reopened)->get("enospc-first");
        const auto second = (*reopened)->get("enospc-second");
        GLIFI_REQUIRE(first.has_value());
        GLIFI_REQUIRE(second.has_value());
        GLIFI_REQUIRE(owned_text(*first) == "first-value");
        GLIFI_REQUIRE(owned_text(*second) == "second-value");
    }
}

// ADR 0040: fault during paced private staging leaves Mold sole authority (no intent).
GLIFI_TEST("paced compaction write_record fault leaves Mold without intent") {
    RecoveryTemporaryDirectory temporary;
    const auto store_id = recovery_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto first = create_segment(*directory, store_id, entries[0]);
        append_record(first, 1, "paced-a", "alpha");
        GLIFI_REQUIRE(first.seal().committed());
        auto second = create_segment(*directory, store_id, entries[1]);
        append_record(second, 2, "paced-b", "beta");
        GLIFI_REQUIRE(second.seal().committed());
        static_cast<void>(create_segment(*directory, store_id, entries[2]));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
    }

    OneShotFilesystemFailure failure{.target = glifistore::FilesystemOperation::write_record,
                                     .code = glifistore::ErrorCode::io_error};
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(),
        glifistore::FilesystemHooks{.context = &failure, .before = &OneShotFilesystemFailure::before});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
    GLIFI_REQUIRE(runtime.has_value());
    // Tiny rate forces paced private staging before intent (ADR 0040).
    const auto result = (*runtime)->compact_worker(0, 0, 0, 1'000);
    GLIFI_REQUIRE(failure.fired);
    GLIFI_REQUIRE(!result.compacted());
    GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::not_compacted);
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kCompactionIntentFilename));
    GLIFI_REQUIRE((*runtime)->manifest() == recovery_manifest(store_id, 1, entries));
    runtime->reset();

    auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
    GLIFI_REQUIRE(owned_text(*(*reopened)->get("paced-a")) == "alpha");
    GLIFI_REQUIRE(owned_text(*(*reopened)->get("paced-b")) == "beta");
}
