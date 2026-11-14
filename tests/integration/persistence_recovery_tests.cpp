#include "persistence_recovery_test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
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

GLIFI_TEST("durable recovery rebuilds partitioned visibility and Worker sequences") {
    RecoveryTemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    const auto store_id = recovery_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{3},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{1},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    const auto alpha = key_for_worker(0, 2, "alpha");
    const auto gone = key_for_worker(0, 2, "gone");
    const auto expired = key_for_worker(0, 2, "expired");
    const std::string binary_prefix{"binary\0key", 10};
    const auto binary = key_for_worker(0, 2, binary_prefix);
    const auto beta = key_for_worker(1, 2, "beta");

    auto first = create_segment(*directory, store_id, entries[0]);
    append_record(first, 1, expired, "older-visible");
    append_record(first, 2, gone, "present");
    append_record(first, 3, gone, {}, glifistore::Opcode::erase);
    append_record(first, 4, alpha, "old");
    append_record(first, 5, binary, "binary-value");
    GLIFI_REQUIRE(first.seal().committed());

    auto second = create_segment(*directory, store_id, entries[1]);
    append_record(second, 6, alpha, "new");
    append_record(second, 7, expired, "stale", glifistore::Opcode::put, 100);

    auto third = create_segment(*directory, store_id, entries[2]);
    append_record(third, 9, beta, "visible");
    GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 2, entries)).durable());

    const auto recovered = glifistore::recover_durable_state(*directory, 101);
    GLIFI_REQUIRE(recovered.has_value());
    GLIFI_REQUIRE(recovered->segments.size() == 3);
    GLIFI_REQUIRE(recovered->workers.size() == 2);
    GLIFI_REQUIRE(recovered->stats.segments_scanned == 3);
    GLIFI_REQUIRE(recovered->stats.rebuild.records_scanned == 8);
    GLIFI_REQUIRE(recovered->stats.rebuild.records_visible == 3);
    GLIFI_REQUIRE(recovered->stats.rebuild.tombstones == 1);
    GLIFI_REQUIRE(recovered->stats.rebuild.expired == 1);
    GLIFI_REQUIRE(recovered->stats.workers_requiring_rotation == 0);

    const auto alpha_ref = recovered->workers[0].index.find(alpha);
    GLIFI_REQUIRE(alpha_ref.has_value());
    GLIFI_REQUIRE(alpha_ref->segment_id.value == 2);
    GLIFI_REQUIRE(alpha_ref->sequence.value == 6);
    GLIFI_REQUIRE(!recovered->workers[0].index.find(gone).has_value());
    GLIFI_REQUIRE(!recovered->workers[0].index.find(expired).has_value());
    GLIFI_REQUIRE(recovered->workers[0].index.find(binary).has_value());
    GLIFI_REQUIRE(recovered->workers[0].next_sequence.value == 8);
    GLIFI_REQUIRE(recovered->workers[0].active_segment.value == 2);

    const auto beta_ref = recovered->workers[1].index.find(beta);
    GLIFI_REQUIRE(beta_ref.has_value());
    GLIFI_REQUIRE(beta_ref->sequence.value == 9);
    GLIFI_REQUIRE(recovered->workers[1].next_sequence.value == 10);
    GLIFI_REQUIRE(recovered->workers[1].active_segment.value == 3);
}

GLIFI_TEST("recovery cleans crash temporaries but rejects unlisted Segments without adoption") {
    {
        RecoveryTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto store_id = recovery_store_id();
        const glifistore::ManifestSegmentEntry active{
            .segment_id = glifistore::SegmentId{1},
            .generation = glifistore::GenerationId{1},
            .owner_worker = glifistore::WorkerId{0},
            .role = glifistore::ManifestSegmentRole::active,
        };
        auto segment = create_segment(*directory, store_id, active);
        GLIFI_REQUIRE(segment.identity().segment_id == active.segment_id);
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
        create_private_file(temporary.path() / glifistore::kManifestTemporaryFilename);
        create_private_file(temporary.path() /
                            ('.' + glifistore::segment_filename(segment.identity()) + ".tmp"));

        const auto recovered = glifistore::recover_durable_state(*directory);
        GLIFI_REQUIRE(recovered.has_value());
        GLIFI_REQUIRE(recovered->namespace_audit.clean());
        GLIFI_REQUIRE(recovered->namespace_audit.recovery_safe());
        GLIFI_REQUIRE(!std::filesystem::exists(temporary.path() / glifistore::kManifestTemporaryFilename));
        GLIFI_REQUIRE(!std::filesystem::exists(
            temporary.path() / ('.' + glifistore::segment_filename(segment.identity()) + ".tmp")));
    }
    {
        RecoveryTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto store_id = recovery_store_id();
        const glifistore::ManifestSegmentEntry active{
            .segment_id = glifistore::SegmentId{1},
            .generation = glifistore::GenerationId{1},
            .owner_worker = glifistore::WorkerId{0},
            .role = glifistore::ManifestSegmentRole::active,
        };
        auto segment = create_segment(*directory, store_id, active);
        GLIFI_REQUIRE(segment.identity().generation == active.generation);
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
        const glifistore::SegmentHeaderIdentity orphan{
            .store_id = store_id,
            .segment_id = glifistore::SegmentId{2},
            .generation = glifistore::GenerationId{1},
            .owner_worker = glifistore::WorkerId{0},
        };
        create_private_file(temporary.path() / glifistore::segment_filename(orphan));

        const auto recovered = glifistore::recover_durable_state(*directory);
        GLIFI_REQUIRE(!recovered.has_value());
        GLIFI_REQUIRE(recovered.error().code == glifistore::ErrorCode::corrupted_data);
        GLIFI_REQUIRE(recovered.error().message.find("unlisted Segment") != std::string::npos);
        GLIFI_REQUIRE(std::filesystem::exists(temporary.path() / glifistore::segment_filename(orphan)));
    }
}

GLIFI_TEST("recovery accepts only the documented sealed-active rotation transition") {
    {
        RecoveryTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto store_id = recovery_store_id();
        const glifistore::ManifestSegmentEntry active{
            .segment_id = glifistore::SegmentId{1},
            .generation = glifistore::GenerationId{1},
            .owner_worker = glifistore::WorkerId{0},
            .role = glifistore::ManifestSegmentRole::active,
        };
        auto file = create_segment(*directory, store_id, active);
        GLIFI_REQUIRE(file.seal().committed());
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
        const auto recovered = glifistore::recover_durable_state(*directory);
        GLIFI_REQUIRE(recovered.has_value());
        GLIFI_REQUIRE(recovered->workers[0].active_requires_rotation);
        GLIFI_REQUIRE(recovered->stats.workers_requiring_rotation == 1);
    }

    {
        RecoveryTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto store_id = recovery_store_id();
        const std::vector entries{
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::sealed},
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::active},
        };
        auto incorrectly_active = create_segment(*directory, store_id, entries[0]);
        auto active = create_segment(*directory, store_id, entries[1]);
        static_cast<void>(incorrectly_active);
        static_cast<void>(active);
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
        const auto recovered = glifistore::recover_durable_state(*directory);
        GLIFI_REQUIRE(!recovered.has_value());
        GLIFI_REQUIRE(recovered.error().code == glifistore::ErrorCode::corrupted_data);
    }
}

GLIFI_TEST("recovery rejects missing and identity-mismatched manifest Segments") {
    {
        RecoveryTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto store_id = recovery_store_id();
        const glifistore::ManifestSegmentEntry active{
            .segment_id = glifistore::SegmentId{1},
            .generation = glifistore::GenerationId{1},
            .owner_worker = glifistore::WorkerId{0},
            .role = glifistore::ManifestSegmentRole::active,
        };
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
        const auto recovered = glifistore::recover_durable_state(*directory);
        GLIFI_REQUIRE(!recovered.has_value());
        GLIFI_REQUIRE(recovered.error().code == glifistore::ErrorCode::corrupted_data);
    }

    {
        RecoveryTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto file_store_id = recovery_store_id();
        const auto manifest_store_id = recovery_store_id(std::byte{0x99});
        const glifistore::ManifestSegmentEntry active{
            .segment_id = glifistore::SegmentId{1},
            .generation = glifistore::GenerationId{1},
            .owner_worker = glifistore::WorkerId{0},
            .role = glifistore::ManifestSegmentRole::active,
        };
        auto file = create_segment(*directory, file_store_id, active);
        static_cast<void>(file);
        GLIFI_REQUIRE(
            directory->publish_manifest(recovery_manifest(manifest_store_id, 1, {active})).durable());
        const auto recovered = glifistore::recover_durable_state(*directory);
        GLIFI_REQUIRE(!recovered.has_value());
        GLIFI_REQUIRE(recovered.error().code == glifistore::ErrorCode::corrupted_data);
    }
}

GLIFI_TEST("recovery validates persisted key hashes and Worker routing") {
    {
        RecoveryTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto store_id = recovery_store_id();
        const glifistore::ManifestSegmentEntry active{
            .segment_id = glifistore::SegmentId{1},
            .generation = glifistore::GenerationId{1},
            .owner_worker = glifistore::WorkerId{0},
            .role = glifistore::ManifestSegmentRole::active,
        };
        auto file = create_segment(*directory, store_id, active);
        const auto key = key_for_worker(0, 1, "hash");
        append_record(file, 1, key, "value", glifistore::Opcode::put, 0, glifistore::hash_key(key) ^ 1U);
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
        const auto recovered = glifistore::recover_durable_state(*directory);
        GLIFI_REQUIRE(!recovered.has_value());
        GLIFI_REQUIRE(recovered.error().code == glifistore::ErrorCode::corrupted_data);
    }

    {
        RecoveryTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto store_id = recovery_store_id();
        const std::vector entries{
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::active},
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{1},
                                             .role = glifistore::ManifestSegmentRole::active},
        };
        auto wrong_owner = create_segment(*directory, store_id, entries[0]);
        auto other_active = create_segment(*directory, store_id, entries[1]);
        const auto key = key_for_worker(1, 2, "wrong-owner");
        append_record(wrong_owner, 1, key, "value");
        static_cast<void>(other_active);
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 2, entries)).durable());
        const auto recovered = glifistore::recover_durable_state(*directory);
        GLIFI_REQUIRE(!recovered.has_value());
        GLIFI_REQUIRE(recovered.error().code == glifistore::ErrorCode::corrupted_data);
    }
}

GLIFI_TEST("recovery rejects equal winning sequences and exhausted Worker sequence space") {
    {
        RecoveryTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto store_id = recovery_store_id();
        const std::vector entries{
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::sealed},
            glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                             .generation = glifistore::GenerationId{1},
                                             .owner_worker = glifistore::WorkerId{0},
                                             .role = glifistore::ManifestSegmentRole::active},
        };
        const auto key = key_for_worker(0, 1, "duplicate");
        auto first = create_segment(*directory, store_id, entries[0]);
        append_record(first, 5, key, "first");
        GLIFI_REQUIRE(first.seal().committed());
        auto second = create_segment(*directory, store_id, entries[1]);
        append_record(second, 5, key, "second");
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
        const auto recovered = glifistore::recover_durable_state(*directory);
        GLIFI_REQUIRE(!recovered.has_value());
        GLIFI_REQUIRE(recovered.error().code == glifistore::ErrorCode::corrupted_data);
    }

    {
        RecoveryTemporaryDirectory temporary;
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        const auto store_id = recovery_store_id();
        const glifistore::ManifestSegmentEntry active{
            .segment_id = glifistore::SegmentId{1},
            .generation = glifistore::GenerationId{1},
            .owner_worker = glifistore::WorkerId{0},
            .role = glifistore::ManifestSegmentRole::active,
        };
        auto file = create_segment(*directory, store_id, active);
        append_record(file, std::numeric_limits<std::uint64_t>::max(), "last", "value");
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
        const auto recovered = glifistore::recover_durable_state(*directory);
        GLIFI_REQUIRE(!recovered.has_value());
        GLIFI_REQUIRE(recovered.error().code == glifistore::ErrorCode::arithmetic_overflow);
    }
}

GLIFI_TEST("recovery rejects overlapping sequence ranges across Worker Segments") {
    RecoveryTemporaryDirectory temporary;
    auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(directory.has_value());
    const auto store_id = recovery_store_id();
    const std::vector entries{
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{1},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::sealed},
        glifistore::ManifestSegmentEntry{.segment_id = glifistore::SegmentId{2},
                                         .generation = glifistore::GenerationId{1},
                                         .owner_worker = glifistore::WorkerId{0},
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    auto first = create_segment(*directory, store_id, entries[0]);
    append_record(first, 10, "first", "value");
    GLIFI_REQUIRE(first.seal().committed());
    auto second = create_segment(*directory, store_id, entries[1]);
    append_record(second, 9, "different-key", "value");
    GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());

    const auto recovered = glifistore::recover_durable_state(*directory);
    GLIFI_REQUIRE(!recovered.has_value());
    GLIFI_REQUIRE(recovered.error().code == glifistore::ErrorCode::corrupted_data);
    GLIFI_REQUIRE(recovered.error().message.find("overlaps or reverses") != std::string::npos);
}

GLIFI_TEST("durable runtime materializes recovered Indexes with bounded concurrent reads") {
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
                                         .role = glifistore::ManifestSegmentRole::active},
    };
    const std::string binary_key{"bin\0key", 7};
    {
        auto directory = glifistore::DataDirectory::open_and_lock(temporary.path());
        GLIFI_REQUIRE(directory.has_value());
        auto sealed = create_segment(*directory, store_id, entries[0]);
        append_record(sealed, 1, "alpha", "one");
        append_record(sealed, 2, binary_key, "binary");
        GLIFI_REQUIRE(sealed.seal().committed());
        auto active = create_segment(*directory, store_id, entries[1]);
        append_record(active, 3, "beta", "two");
        append_record(active, 4, "expired", "old", glifistore::Opcode::put, 100);
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
    }

    auto runtime = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
    GLIFI_REQUIRE(runtime.has_value());
    GLIFI_REQUIRE((*runtime)->healthy());
    GLIFI_REQUIRE((*runtime)->worker_count() == 1);
    GLIFI_REQUIRE((*runtime)->manifest().segments.size() == 2);
    GLIFI_REQUIRE((*runtime)->namespace_audit().clean());
    GLIFI_REQUIRE((*runtime)->next_sequence(0).has_value());
    GLIFI_REQUIRE((*runtime)->next_sequence(0)->value == 5);
    GLIFI_REQUIRE((*runtime)->active_segment(0)->value == 2);

    const auto alpha = (*runtime)->get("alpha");
    const auto beta = (*runtime)->get("beta");
    const auto binary = (*runtime)->get(binary_key);
    GLIFI_REQUIRE(alpha.has_value());
    GLIFI_REQUIRE(beta.has_value());
    GLIFI_REQUIRE(binary.has_value());
    GLIFI_REQUIRE(owned_text(*alpha) == "one");
    GLIFI_REQUIRE(owned_text(*beta) == "two");
    GLIFI_REQUIRE(owned_text(*binary) == "binary");
    const auto expired = (*runtime)->get("expired", 100);
    GLIFI_REQUIRE(!expired.has_value());
    GLIFI_REQUIRE(expired.error().code == glifistore::ErrorCode::not_found);

    std::atomic_bool failed{};
    std::vector<std::thread> readers;
    for (std::size_t thread = 0; thread < 8; ++thread) {
        readers.emplace_back([&, thread] {
            for (std::size_t iteration = 0; iteration < 32; ++iteration) {
                const bool choose_alpha = (thread + iteration) % 2 == 0;
                const auto value = (*runtime)->get(choose_alpha ? "alpha" : "beta");
                if (!value || owned_text(*value) != (choose_alpha ? "one" : "two")) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto& reader : readers) {
        reader.join();
    }
    GLIFI_REQUIRE(!failed.load(std::memory_order_relaxed));
    GLIFI_REQUIRE((*runtime)->healthy());

    const auto locked_again = glifistore::DataDirectory::open_and_lock(temporary.path());
    GLIFI_REQUIRE(!locked_again.has_value());
}

GLIFI_TEST("blocked durable cold read does not block a mutation on the same Worker") {
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
        auto segment = create_segment(*directory, store_id, active);
        append_record(segment, 1, "cold", "value");
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, {active})).durable());
    }

    BlockingRecordRead blocked_read;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(),
        glifistore::FilesystemHooks{
            .context = &blocked_read,
            .file_io = {.context = &blocked_read, .read_some_at = &BlockingRecordRead::read_some_at}});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime =
        glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, cold_read_runtime_options());
    GLIFI_REQUIRE(runtime.has_value());
    blocked_read.arm();

    std::optional<glifistore::Result<glifistore::OwnedValue>> read_result;
    std::thread reader{[&] { read_result.emplace((*runtime)->get("cold")); }};
    GLIFI_REQUIRE(blocked_read.wait_until_blocked());

    std::mutex completion_mutex;
    std::condition_variable completion;
    bool mutation_finished{};
    glifistore::DurableMutationResult mutation;
    const std::string other_key{"other"};
    const std::string other_value{"new-value"};
    std::thread writer{[&] {
        mutation =
            (*runtime)->put(std::as_bytes(std::span{other_key}), std::as_bytes(std::span{other_value}));
        {
            const std::lock_guard lock{completion_mutex};
            mutation_finished = true;
        }
        completion.notify_one();
    }};

    bool mutation_completed_while_read_blocked{};
    {
        std::unique_lock lock{completion_mutex};
        mutation_completed_while_read_blocked =
            completion.wait_for(lock, std::chrono::seconds{2}, [&] { return mutation_finished; });
    }
    blocked_read.release();
    writer.join();
    reader.join();

    GLIFI_REQUIRE(mutation_completed_while_read_blocked);
    GLIFI_REQUIRE(mutation.committed());
    GLIFI_REQUIRE(read_result.has_value());
    GLIFI_REQUIRE(read_result->has_value());
    GLIFI_REQUIRE(owned_text(**read_result) == "value");
    const auto written = (*runtime)->get(other_key);
    GLIFI_REQUIRE(written.has_value());
    GLIFI_REQUIRE(owned_text(*written) == other_value);
}

GLIFI_TEST("durable cold read pin survives concurrent source retirement and relinearizes") {
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
        append_record(first, 1, "cold", "value");
        GLIFI_REQUIRE(first.seal().committed());
        auto second = create_segment(*directory, store_id, entries[1]);
        append_record(second, 2, "second", "record");
        GLIFI_REQUIRE(second.seal().committed());
        static_cast<void>(create_segment(*directory, store_id, entries[2]));
        GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
    }

    BlockingRecordRead blocked_read;
    auto directory = glifistore::DataDirectory::open_and_lock(
        temporary.path(),
        glifistore::FilesystemHooks{
            .context = &blocked_read,
            .file_io = {.context = &blocked_read, .read_some_at = &BlockingRecordRead::read_some_at}});
    GLIFI_REQUIRE(directory.has_value());
    auto runtime =
        glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory), 0, cold_read_runtime_options());
    GLIFI_REQUIRE(runtime.has_value());
    blocked_read.arm();

    std::optional<glifistore::Result<glifistore::OwnedValue>> read_result;
    std::thread reader{[&] { read_result.emplace((*runtime)->get("cold")); }};
    GLIFI_REQUIRE(blocked_read.wait_until_blocked());

    std::mutex completion_mutex;
    std::condition_variable completion;
    bool compaction_finished{};
    glifistore::DurableCompactionResult compaction;
    std::thread compactor{[&] {
        compaction = (*runtime)->compact_worker(0, 0);
        {
            const std::lock_guard lock{completion_mutex};
            compaction_finished = true;
        }
        completion.notify_one();
    }};
    bool retired_while_read_blocked{};
    {
        std::unique_lock lock{completion_mutex};
        retired_while_read_blocked =
            completion.wait_for(lock, std::chrono::seconds{5}, [&] { return compaction_finished; });
    }
    blocked_read.release();
    compactor.join();
    reader.join();

    GLIFI_REQUIRE(retired_while_read_blocked);
    GLIFI_REQUIRE(compaction.compacted());
    GLIFI_REQUIRE(read_result.has_value());
    GLIFI_REQUIRE(read_result->has_value());
    GLIFI_REQUIRE(owned_text(**read_result) == "value");
    GLIFI_REQUIRE((*runtime)->manifest().segments.size() == 2);
}
