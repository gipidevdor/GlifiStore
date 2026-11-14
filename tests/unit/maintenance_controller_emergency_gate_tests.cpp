#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/store/maintenance.hpp"
#include "glifistore/store/store.hpp"
#include "maintenance_controller_test_support.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

// Maintenance emergency mutation gate + batch TOCTOU litmus (split for structure budget).

GLIFI_TEST("emergency rejects put and erase with storage_exhausted") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 60'000;
    config.maintenance.max_eval_interval_ms = 60'000;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    // Isolate the emergency-gate contract from the initial background
    // compaction evaluation. A paired volatile mutation racing that evaluation
    // may legitimately observe the Index-quiesce sequence_conflict covered by
    // the dedicated compaction/PUT concurrency test.
    static_cast<void>(wait_for_initial_idle(**store));
    GLIFI_REQUIRE((**store).put("alive", std::as_bytes(std::span{"v", 1})).has_value());

    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);
    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 100,
                .sealed_segment_count = 2,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 2'048,
            };
        });
    controller->request_evaluate();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snap = (**store).maintenance_snapshot();
        if (snap.mutations_rejected &&
            snap.last_activation_reason == glifistore::MaintenanceActivationReason::emergency_capacity) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto snap = (**store).maintenance_snapshot();
    GLIFI_REQUIRE(snap.pressure == glifistore::MaintenancePressureLevel::emergency);
    GLIFI_REQUIRE(snap.mutations_rejected);
    GLIFI_REQUIRE(snap.last_activation_reason == glifistore::MaintenanceActivationReason::emergency_capacity);

    const auto put = (**store).put("blocked", std::as_bytes(std::span{"x", 1}));
    GLIFI_REQUIRE(!put.has_value());
    GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::storage_exhausted);
    GLIFI_REQUIRE(put.error().message == glifistore::kMaintenanceEmergencyMutationMessage);

    const auto erased = (**store).erase("alive");
    GLIFI_REQUIRE(!erased.has_value());
    GLIFI_REQUIRE(erased.error().code == glifistore::ErrorCode::storage_exhausted);

    // Reads and compact remain available under emergency.
    const auto got = (**store).get("alive");
    GLIFI_REQUIRE(got.has_value());
    const auto compacted = (**store).compact();
    GLIFI_REQUIRE(compacted.has_value() ||
                  compacted.error().code == glifistore::ErrorCode::sequence_conflict);

    // Recovery: observation clears emergency → mutations resume.
    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 10,
                .sealed_segment_count = 1,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
            };
        });
    controller->request_evaluate();
    const auto recover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < recover_deadline) {
        if (!(**store).maintenance_snapshot().mutations_rejected) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(!(**store).maintenance_snapshot().mutations_rejected);
    GLIFI_REQUIRE((**store).put("recovered", std::as_bytes(std::span{"y", 1})).has_value());
    GLIFI_REQUIRE((**store).close().has_value());
}

GLIFI_TEST("caller_holds_guard still rejects maintenance emergency") {
    // Paired sync Writer uses caller_holds_guard (skips nested OperationGuard).
    // Emergency must still reject before append — Store::put's outer check is not
    // enough when the gate arms mid-batch / between admit and Writer apply.
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.concurrency = glifistore::StoreConcurrencyMode::paired;
    config.paired = {
        .async_lane_capacity = 8, .async_lane_payload_bytes = 1U * 1024U * 1024U, .reader_epoch_lease = true};
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 60'000;
    config.maintenance.max_eval_interval_ms = 60'000;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);
    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 100,
                .sealed_segment_count = 2,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 2'048,
            };
        });
    controller->request_evaluate();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if ((**store).maintenance_snapshot().mutations_rejected) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE((**store).maintenance_snapshot().mutations_rejected);

    constexpr std::string_view key = "guard-bypass";
    const glifistore::HashedKey hashed{key, glifistore::hash_key_routing(key)};
    const auto value = std::as_bytes(std::span{"x", 1});
    const auto published = glifistore::detail::StoreAccess::put_volatile_published(
        **store, 0, hashed, value, 0,
        glifistore::detail::StoreAccess::PublishedAdmission::caller_holds_guard);
    GLIFI_REQUIRE(!published.has_value());
    GLIFI_REQUIRE(published.error().code == glifistore::ErrorCode::storage_exhausted);
    GLIFI_REQUIRE(published.error().message == glifistore::kMaintenanceEmergencyMutationMessage);
    GLIFI_REQUIRE(!(**store).get(key).has_value());

    const auto erased = glifistore::detail::StoreAccess::erase_volatile_published(
        **store, 0, hashed, glifistore::detail::StoreAccess::PublishedAdmission::caller_holds_guard);
    GLIFI_REQUIRE(!erased.has_value());
    GLIFI_REQUIRE(erased.error().code == glifistore::ErrorCode::storage_exhausted);

    GLIFI_REQUIRE((**store).close().has_value());
}

GLIFI_TEST("mutate_durable_batch rejects under maintenance emergency") {
    // Batch-entry gate: armed emergency must reject every sibling before append.
    // legacy_mutex: white-box StoreAccess mutate must be Index-visible via Store::get
    // (paired GET reads a generation snapshot, not the Writer Index directly).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-durable-batch-gate-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(store);
    GLIFI_REQUIRE(controller != nullptr);
    // No background eval to clear a force-published gate.
    controller->publish_mutations_rejected(true);
    GLIFI_REQUIRE(store.maintenance_snapshot().mutations_rejected);

    constexpr std::string_view key_a = "batch-a";
    constexpr std::string_view key_b = "batch-b";
    const auto routing = glifistore::detail::StoreAccess::worker_routing(store);
    const glifistore::HashedKey hashed_a{key_a, glifistore::hash_key_routing(key_a, routing)};
    const glifistore::HashedKey hashed_b{key_b, glifistore::hash_key_routing(key_b, routing)};
    const auto value = std::as_bytes(std::span{"x", 1});
    const glifistore::detail::StoreAccess::DurableMutationView views[] = {
        {.operation = glifistore::detail::StoreAccess::MutationOperation::put,
         .key = hashed_a,
         .value = value,
         .expire_at_ns = 0},
        {.operation = glifistore::detail::StoreAccess::MutationOperation::put,
         .key = hashed_b,
         .value = value,
         .expire_at_ns = 0},
    };
    const auto results = glifistore::detail::StoreAccess::mutate_durable_batch(store, 0, views);
    GLIFI_REQUIRE(results.size() == 2);
    for (const auto& result : results) {
        GLIFI_REQUIRE(result.mutation.outcome == glifistore::DurableMutationOutcome::not_committed);
        GLIFI_REQUIRE(result.mutation.error.has_value());
        GLIFI_REQUIRE(result.mutation.error->code == glifistore::ErrorCode::storage_exhausted);
        GLIFI_REQUIRE(result.mutation.error->message == glifistore::kMaintenanceEmergencyMutationMessage);
    }
    GLIFI_REQUIRE(!store.get(key_a).has_value());
    GLIFI_REQUIRE(!store.get(key_b).has_value());
    GLIFI_REQUIRE(store.close().has_value());
    std::filesystem::remove_all(root);
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("mutate_durable_batch mid-batch TOCTOU rejects later siblings") {
    // Gate arms after sibling 0's entry into the loop: sibling 1+ must still
    // reject before append (maintenance-controller.md mid-batch contract).
    auto pattern =
        (std::filesystem::temp_directory_path() / "glifistore-durable-batch-toctou-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open(
        {.worker_config = {.explicit_count = 1},
         .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
         .storage_mode = glifistore::StorageMode::durable_group,
         .data_directory = store_path,
         .durable_open_mode = glifistore::DurableOpenMode::create_new,
         .durable_group = {.max_records = 32, .max_bytes = 65'536, .max_wait_ms = 10, .min_records = 1},
         .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(!store.maintenance_snapshot().mutations_rejected);

    glifistore::fault::reset();
    glifistore::fault::configure(1, 0, 0);
    // First sibling proceeds; second consume arms the gate before mutate.
    glifistore::fault::fail_nth(glifistore::fault::Site::durable_batch_gate, 2);

    constexpr std::string_view key_a = "toctou-a";
    constexpr std::string_view key_b = "toctou-b";
    const auto routing = glifistore::detail::StoreAccess::worker_routing(store);
    const glifistore::HashedKey hashed_a{key_a, glifistore::hash_key_routing(key_a, routing)};
    const glifistore::HashedKey hashed_b{key_b, glifistore::hash_key_routing(key_b, routing)};
    const auto value = std::as_bytes(std::span{"y", 1});
    const glifistore::detail::StoreAccess::DurableMutationView views[] = {
        {.operation = glifistore::detail::StoreAccess::MutationOperation::put,
         .key = hashed_a,
         .value = value,
         .expire_at_ns = 0},
        {.operation = glifistore::detail::StoreAccess::MutationOperation::put,
         .key = hashed_b,
         .value = value,
         .expire_at_ns = 0},
    };
    const auto results = glifistore::detail::StoreAccess::mutate_durable_batch(store, 0, views);
    GLIFI_REQUIRE(results.size() == 2);
    GLIFI_REQUIRE(results[0].mutation.committed());
    GLIFI_REQUIRE(!results[0].mutation.error.has_value());
    GLIFI_REQUIRE(results[1].mutation.outcome == glifistore::DurableMutationOutcome::not_committed);
    GLIFI_REQUIRE(results[1].mutation.error.has_value());
    GLIFI_REQUIRE(results[1].mutation.error->code == glifistore::ErrorCode::storage_exhausted);
    GLIFI_REQUIRE(results[1].mutation.error->message == glifistore::kMaintenanceEmergencyMutationMessage);
    GLIFI_REQUIRE(store.maintenance_snapshot().mutations_rejected);

    const auto got_a = store.get(key_a);
    GLIFI_REQUIRE(got_a.has_value());
    GLIFI_REQUIRE(!store.get(key_b).has_value());
    GLIFI_REQUIRE(store.close().has_value());

    auto reopened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                             .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
                                             .storage_mode = glifistore::StorageMode::durable_group,
                                             .data_directory = store_path,
                                             .durable_open_mode = glifistore::DurableOpenMode::open_existing,
                                             .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->get(key_a).has_value());
    GLIFI_REQUIRE(!(*reopened)->get(key_b).has_value());
    GLIFI_REQUIRE((*reopened)->close().has_value());
    glifistore::fault::reset();
    std::filesystem::remove_all(root);
}
#endif

GLIFI_TEST("put_batch rejects under maintenance emergency") {
    // Batch-entry gate on the non-paired put_batch path.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-put-batch-gate-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = store_path,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(store);
    GLIFI_REQUIRE(controller != nullptr);
    controller->publish_mutations_rejected(true);
    GLIFI_REQUIRE(store.maintenance_snapshot().mutations_rejected);

    const auto value = std::as_bytes(std::span{"x", 1});
    const glifistore::Store::PutItem items[] = {
        {.key = "batch-a", .value = value},
        {.key = "batch-b", .value = value},
    };
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == 2);
    for (const auto& status : statuses) {
        GLIFI_REQUIRE(!status.has_value());
        GLIFI_REQUIRE(status.error().code == glifistore::ErrorCode::storage_exhausted);
        GLIFI_REQUIRE(status.error().message == glifistore::kMaintenanceEmergencyMutationMessage);
    }
    GLIFI_REQUIRE(!store.get("batch-a").has_value());
    GLIFI_REQUIRE(!store.get("batch-b").has_value());
    GLIFI_REQUIRE(store.close().has_value());
    std::filesystem::remove_all(root);
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("put_batch mid-batch TOCTOU rejects later siblings") {
    // Non-paired put_batch: gate arms after item 0; item 1+ must reject before append.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-put-batch-toctou-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = store_path,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    GLIFI_REQUIRE(!store.maintenance_snapshot().mutations_rejected);

    glifistore::fault::reset();
    glifistore::fault::configure(1, 0, 0);
    glifistore::fault::fail_nth(glifistore::fault::Site::put_batch_gate, 2);

    const auto value = std::as_bytes(std::span{"y", 1});
    const glifistore::Store::PutItem items[] = {
        {.key = "toctou-a", .value = value},
        {.key = "toctou-b", .value = value},
    };
    const auto statuses = store.put_batch(items);
    GLIFI_REQUIRE(statuses.size() == 2);
    GLIFI_REQUIRE(statuses[0].has_value());
    GLIFI_REQUIRE(!statuses[1].has_value());
    GLIFI_REQUIRE(statuses[1].error().code == glifistore::ErrorCode::storage_exhausted);
    GLIFI_REQUIRE(statuses[1].error().message == glifistore::kMaintenanceEmergencyMutationMessage);
    GLIFI_REQUIRE(store.maintenance_snapshot().mutations_rejected);

    GLIFI_REQUIRE(store.get("toctou-a").has_value());
    GLIFI_REQUIRE(!store.get("toctou-b").has_value());
    GLIFI_REQUIRE(store.close().has_value());

    auto reopened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                             .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
                                             .storage_mode = glifistore::StorageMode::durable_sync,
                                             .data_directory = store_path,
                                             .durable_open_mode = glifistore::DurableOpenMode::open_existing,
                                             .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE((*reopened)->get("toctou-a").has_value());
    GLIFI_REQUIRE(!(*reopened)->get("toctou-b").has_value());
    GLIFI_REQUIRE((*reopened)->close().has_value());
    glifistore::fault::reset();
    std::filesystem::remove_all(root);
}
#endif

GLIFI_TEST("erase_batch rejects under maintenance emergency") {
    // Batch-entry gate on the non-paired erase_batch path (parity with put_batch).
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-erase-batch-gate-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = store_path,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    const auto value = std::as_bytes(std::span{"x", 1});
    GLIFI_REQUIRE(store.put("batch-a", value).has_value());
    GLIFI_REQUIRE(store.put("batch-b", value).has_value());

    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(store);
    GLIFI_REQUIRE(controller != nullptr);
    controller->publish_mutations_rejected(true);
    GLIFI_REQUIRE(store.maintenance_snapshot().mutations_rejected);

    const glifistore::Store::EraseItem items[] = {
        {.key = "batch-a"},
        {.key = "batch-b"},
    };
    const auto statuses = store.erase_batch(items);
    GLIFI_REQUIRE(statuses.size() == 2);
    for (const auto& status : statuses) {
        GLIFI_REQUIRE(!status.has_value());
        GLIFI_REQUIRE(status.error().code == glifistore::ErrorCode::storage_exhausted);
        GLIFI_REQUIRE(status.error().message == glifistore::kMaintenanceEmergencyMutationMessage);
    }
    GLIFI_REQUIRE(store.get("batch-a").has_value());
    GLIFI_REQUIRE(store.get("batch-b").has_value());
    GLIFI_REQUIRE(store.close().has_value());
    std::filesystem::remove_all(root);
}

#if defined(GLIFISTORE_FAULT_INJECTION)
GLIFI_TEST("erase_batch mid-batch TOCTOU rejects later siblings") {
    // Non-paired erase_batch reuses Site::put_batch_gate: arm after item 0; item 1+ reject.
    auto pattern = (std::filesystem::temp_directory_path() / "glifistore-erase-batch-toctou-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    GLIFI_REQUIRE(::mkdtemp(writable.data()) != nullptr);
    const std::filesystem::path root{writable.data()};
    const auto store_path = root / "store";

    auto opened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                           .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
                                           .storage_mode = glifistore::StorageMode::durable_sync,
                                           .data_directory = store_path,
                                           .durable_open_mode = glifistore::DurableOpenMode::create_new,
                                           .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(opened.has_value());
    auto& store = **opened;
    const auto value = std::as_bytes(std::span{"y", 1});
    GLIFI_REQUIRE(store.put("toctou-a", value).has_value());
    GLIFI_REQUIRE(store.put("toctou-b", value).has_value());
    GLIFI_REQUIRE(!store.maintenance_snapshot().mutations_rejected);

    glifistore::fault::reset();
    glifistore::fault::configure(1, 0, 0);
    glifistore::fault::fail_nth(glifistore::fault::Site::put_batch_gate, 2);

    const glifistore::Store::EraseItem items[] = {
        {.key = "toctou-a"},
        {.key = "toctou-b"},
    };
    const auto statuses = store.erase_batch(items);
    GLIFI_REQUIRE(statuses.size() == 2);
    GLIFI_REQUIRE(statuses[0].has_value());
    GLIFI_REQUIRE(!statuses[1].has_value());
    GLIFI_REQUIRE(statuses[1].error().code == glifistore::ErrorCode::storage_exhausted);
    GLIFI_REQUIRE(statuses[1].error().message == glifistore::kMaintenanceEmergencyMutationMessage);
    GLIFI_REQUIRE(store.maintenance_snapshot().mutations_rejected);

    GLIFI_REQUIRE(!store.get("toctou-a").has_value());
    GLIFI_REQUIRE(store.get("toctou-b").has_value());
    GLIFI_REQUIRE(store.close().has_value());

    auto reopened = glifistore::Store::open({.worker_config = {.explicit_count = 1},
                                             .concurrency = glifistore::StoreConcurrencyMode::legacy_mutex,
                                             .storage_mode = glifistore::StorageMode::durable_sync,
                                             .data_directory = store_path,
                                             .durable_open_mode = glifistore::DurableOpenMode::open_existing,
                                             .maintenance = {.mode = glifistore::MaintenanceMode::disabled}});
    GLIFI_REQUIRE(reopened.has_value());
    GLIFI_REQUIRE(!(*reopened)->get("toctou-a").has_value());
    GLIFI_REQUIRE((*reopened)->get("toctou-b").has_value());
    GLIFI_REQUIRE((*reopened)->close().has_value());
    glifistore::fault::reset();
    std::filesystem::remove_all(root);
}
#endif

GLIFI_TEST("emergency rejects mutations even when auto-compact is disabled") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 60'000;
    config.maintenance.max_eval_interval_ms = 60'000;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);

    const auto initial = wait_for_initial_idle(**store);
    controller->set_auto_compact_enabled(false);
    const auto baseline_cycles = initial.evaluation_cycles;
    const auto baseline_attempts = initial.compact_attempts;
    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 4,
                .sealed_segment_count = 0,
                .max_segment_count = 8,
                .reserved_free_bytes = 100,
                .available_free_bytes = 50,
            };
        });
    controller->request_evaluate();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snap = (**store).maintenance_snapshot();
        if (snap.evaluation_cycles > baseline_cycles && snap.mutations_rejected) {
            GLIFI_REQUIRE(snap.pressure == glifistore::MaintenancePressureLevel::emergency);
            GLIFI_REQUIRE(snap.compact_attempts == baseline_attempts);
            GLIFI_REQUIRE(snap.last_skip_reason == glifistore::MaintenanceSkipReason::policy_deferred);
            const auto put = (**store).put("x", std::as_bytes(std::span{"z", 1}));
            GLIFI_REQUIRE(!put.has_value());
            GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::storage_exhausted);
            GLIFI_REQUIRE((**store).close().has_value());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("emergency gate survives compact fault") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 60'000;
    config.maintenance.max_eval_interval_ms = 60'000;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);

    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 100,
                .sealed_segment_count = 2,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 2'048,
            };
        });
    controller->bind_compact(
        [](std::optional<std::size_t>, std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            return glifistore::fail(glifistore::ErrorCode::io_error, "injected compact fault");
        });
    controller->request_evaluate();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snap = (**store).maintenance_snapshot();
        if (snap.mutations_rejected && snap.last_error.has_value() &&
            snap.last_error->code == glifistore::ErrorCode::io_error) {
            GLIFI_REQUIRE(snap.state == glifistore::MaintenanceState::faulted || snap.compact_attempts > 0);
            GLIFI_REQUIRE(snap.pressure == glifistore::MaintenancePressureLevel::emergency);
            const auto put = (**store).put("still-blocked", std::as_bytes(std::span{"z", 1}));
            GLIFI_REQUIRE(!put.has_value());
            GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::storage_exhausted);

            controller->bind_observe([](glifistore::MaintenanceObserveRequest)
                                         -> glifistore::Result<glifistore::MaintenanceObservation> {
                return glifistore::MaintenanceObservation{
                    .durable = true,
                    .segment_count = 10,
                    .sealed_segment_count = 1,
                    .max_segment_count = 100,
                    .reserved_free_bytes = 1'024,
                    .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
                };
            });
            controller->request_evaluate();
            const auto recover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (std::chrono::steady_clock::now() < recover_deadline) {
                if (!(**store).maintenance_snapshot().mutations_rejected) {
                    GLIFI_REQUIRE((**store).put("after-fault", std::as_bytes(std::span{"y", 1})).has_value());
                    GLIFI_REQUIRE((**store).close().has_value());
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            GLIFI_REQUIRE(false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("close under emergency clears mutations_rejected and stops thread") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 60'000;
    config.maintenance.max_eval_interval_ms = 60'000;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);
    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 100,
                .sealed_segment_count = 1,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 2'048,
            };
        });
    controller->request_evaluate();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if ((**store).maintenance_snapshot().mutations_rejected) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE((**store).maintenance_snapshot().mutations_rejected);
    GLIFI_REQUIRE((**store).close().has_value());
    const auto snap = (**store).maintenance_snapshot();
    GLIFI_REQUIRE(!snap.thread_running);
    GLIFI_REQUIRE(!snap.mutations_rejected);
    GLIFI_REQUIRE(snap.state == glifistore::MaintenanceState::stopped);
}

GLIFI_TEST("flush succeeds while emergency rejects put") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 60'000;
    config.maintenance.max_eval_interval_ms = 60'000;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);
    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 100,
                .sealed_segment_count = 0,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 2'048,
            };
        });
    controller->request_evaluate();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if ((**store).maintenance_snapshot().mutations_rejected) {
            GLIFI_REQUIRE((**store).flush().has_value());
            const auto put = (**store).put("nope", std::as_bytes(std::span{"n", 1}));
            GLIFI_REQUIRE(!put.has_value());
            GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::storage_exhausted);
            GLIFI_REQUIRE((**store).close().has_value());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("emergency compact fault keeps reclaim attempts while gate is armed") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 60'000;
    config.maintenance.max_eval_interval_ms = 60'000;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);
    static_cast<void>(wait_for_initial_idle(**store));

    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 100,
                .sealed_segment_count = 2,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 2'048,
            };
        });
    controller->bind_compact(
        [compact_calls](std::optional<std::size_t>,
                        std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            compact_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::fail(glifistore::ErrorCode::storage_exhausted, "injected reclaim fault");
        });

    controller->request_evaluate();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = (**store).maintenance_snapshot();
        if (compact_calls->load(std::memory_order_relaxed) >= 1 && snapshot.mutations_rejected &&
            snapshot.state == glifistore::MaintenanceState::faulted) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto first_fault = (**store).maintenance_snapshot();
    GLIFI_REQUIRE(first_fault.mutations_rejected);
    GLIFI_REQUIRE(first_fault.state == glifistore::MaintenanceState::faulted);
    const auto attempts_after_first = first_fault.compact_attempts;
    GLIFI_REQUIRE(attempts_after_first >= 1);

    controller->request_evaluate();
    const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < retry_deadline) {
        if ((**store).maintenance_snapshot().compact_attempts > attempts_after_first) {
            GLIFI_REQUIRE((**store).maintenance_snapshot().mutations_rejected);
            const auto put = (**store).put("still-gated", std::as_bytes(std::span{"z", 1}));
            GLIFI_REQUIRE(!put.has_value());
            GLIFI_REQUIRE(put.error().code == glifistore::ErrorCode::storage_exhausted);
            GLIFI_REQUIRE((**store).close().has_value());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}
