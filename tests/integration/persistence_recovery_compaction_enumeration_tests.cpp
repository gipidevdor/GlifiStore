#include "persistence_recovery_test_support.hpp"

#include <cstddef>
#include <utility>
#include <vector>

GLIFI_TEST("online compaction enumerates every reached filesystem failure point") {
    constexpr std::size_t kMaximumEnumeratedFailurePoints = 64U;
    bool reached_successful_terminal_iteration{};
    std::size_t exercised_failure_points{};

    for (std::size_t failure_index = 1U; failure_index <= kMaximumEnumeratedFailurePoints; ++failure_index) {
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
            append_record(first, 1, "enumerated-a", "alpha");
            GLIFI_REQUIRE(first.seal().committed());
            auto second = create_segment(*directory, store_id, entries[1]);
            append_record(second, 2, "enumerated-b", "beta");
            GLIFI_REQUIRE(second.seal().committed());
            static_cast<void>(create_segment(*directory, store_id, entries[2]));
            GLIFI_REQUIRE(directory->publish_manifest(recovery_manifest(store_id, 1, entries)).durable());
        }

        NthFilesystemFailure failure{.target_occurrence = failure_index};
        auto directory = glifistore::DataDirectory::open_and_lock(
            temporary.path(),
            glifistore::FilesystemHooks{.context = &failure, .before = &NthFilesystemFailure::before});
        GLIFI_REQUIRE(directory.has_value());
        auto runtime = glifistore::DurableRuntimeCatalog::open_locked(std::move(*directory));
        GLIFI_REQUIRE(runtime.has_value());
        const auto result = (*runtime)->compact_worker(0, 0);

        if (!failure.fired_operation) {
            GLIFI_REQUIRE(result.compacted());
            GLIFI_REQUIRE(failure_index == failure.occurrences + 1U);
            reached_successful_terminal_iteration = true;
            runtime->reset();
            break;
        }

        ++exercised_failure_points;
        GLIFI_REQUIRE(failure.occurrences == failure_index);
        GLIFI_REQUIRE(!result.compacted());
        GLIFI_REQUIRE(result.error.has_value());
        GLIFI_REQUIRE(result.error->code == glifistore::ErrorCode::io_error);
        GLIFI_REQUIRE(result.outcome == glifistore::DurableCompactionOutcome::not_compacted ||
                      result.outcome == glifistore::DurableCompactionOutcome::recovery_required);
        runtime->reset();

        // Reopen is the persisted-state oracle: it must select one clean
        // authority, release every temporary authority/descriptor, and retain
        // both logical records regardless of the failed boundary.
        auto reopened = glifistore::DurableRuntimeCatalog::open_existing(temporary.path());
        GLIFI_REQUIRE(reopened.has_value());
        GLIFI_REQUIRE((*reopened)->healthy());
        GLIFI_REQUIRE((*reopened)->namespace_audit().clean());
        const auto first = (*reopened)->get("enumerated-a");
        const auto second = (*reopened)->get("enumerated-b");
        GLIFI_REQUIRE(first.has_value());
        GLIFI_REQUIRE(second.has_value());
        GLIFI_REQUIRE(owned_text(*first) == "alpha");
        GLIFI_REQUIRE(owned_text(*second) == "beta");
    }

    GLIFI_REQUIRE(reached_successful_terminal_iteration);
    GLIFI_REQUIRE(exercised_failure_points > 0U);
}
