#include "glifistore/core/fault_injection.hpp"
#include "glifistore/core/key_hash.hpp"
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

GLIFI_TEST("classify_maintenance_pressure detects segment and free-space watermarks") {
    glifistore::MaintenanceConfig config{};
    config.segment_count_pressure_pct = 80;
    config.free_bytes_pressure_margin = glifistore::kSegmentSizeBytes + 1'000ULL;

    glifistore::MaintenanceObservation ok{
        .durable = true,
        .segment_count = 10,
        .sealed_segment_count = 2,
        .max_segment_count = 100,
        .reserved_free_bytes = 256,
        .available_free_bytes = glifistore::kSegmentSizeBytes + 10'000ULL,
    };
    GLIFI_REQUIRE(glifistore::classify_maintenance_pressure(ok, config) ==
                  glifistore::MaintenancePressureLevel::normal);

    glifistore::MaintenanceObservation segments = ok;
    segments.segment_count = 80;
    GLIFI_REQUIRE(glifistore::classify_maintenance_pressure(segments, config) ==
                  glifistore::MaintenancePressureLevel::pressure);

    glifistore::MaintenanceObservation free_space = ok;
    // Pressure watermark without emergency: above reserved+Segment, at/under reserved+margin.
    free_space.available_free_bytes = free_space.reserved_free_bytes + glifistore::kSegmentSizeBytes;
    GLIFI_REQUIRE(glifistore::classify_maintenance_pressure(free_space, config) ==
                  glifistore::MaintenancePressureLevel::pressure);

    glifistore::MaintenanceObservation emergency_segments = ok;
    emergency_segments.segment_count = 100;
    GLIFI_REQUIRE(glifistore::classify_maintenance_pressure(emergency_segments, config) ==
                  glifistore::MaintenancePressureLevel::emergency);

    glifistore::MaintenanceObservation emergency_free = ok;
    emergency_free.available_free_bytes =
        emergency_free.reserved_free_bytes + glifistore::kSegmentSizeBytes - 1ULL;
    GLIFI_REQUIRE(glifistore::classify_maintenance_pressure(emergency_free, config) ==
                  glifistore::MaintenancePressureLevel::emergency);

    glifistore::MaintenanceObservation rotate_headroom = ok;
    rotate_headroom.rotate_additional_bytes = glifistore::kSegmentSizeBytes + 4'096ULL;
    rotate_headroom.available_free_bytes =
        rotate_headroom.reserved_free_bytes + glifistore::kSegmentSizeBytes;
    GLIFI_REQUIRE(glifistore::classify_maintenance_pressure(rotate_headroom, config) ==
                  glifistore::MaintenancePressureLevel::emergency);
}

GLIFI_TEST("pressure policy continues compacting despite no-gain budget") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 60'000;
    config.maintenance.max_eval_interval_ms = 60'000;
    config.maintenance.max_no_gain_attempts = 1;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);

    // First eval: volatile no-gain increments streak to 1 (at budget limit).
    controller->request_evaluate();
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            if ((**store).maintenance_snapshot().compact_attempts > 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    GLIFI_REQUIRE((**store).maintenance_snapshot().compact_attempts >= 1);

    // Inject durable pressure (not emergency) with sealed history so budget would suspend under
    // normal, but pressure must still attempt compact.
    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 90,
                .sealed_segment_count = 2,
                .max_segment_count = 100,
                .reserved_free_bytes = 100,
                .available_free_bytes = 100ULL + glifistore::kSegmentSizeBytes,
            };
        });
    const auto attempts_before = (**store).maintenance_snapshot().compact_attempts;
    controller->request_evaluate();
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto snap = (**store).maintenance_snapshot();
            if (snap.compact_attempts > attempts_before) {
                GLIFI_REQUIRE(snap.pressure == glifistore::MaintenancePressureLevel::pressure);
                GLIFI_REQUIRE(!snap.mutations_rejected);
                GLIFI_REQUIRE(snap.last_activation_reason ==
                                  glifistore::MaintenanceActivationReason::segment_pressure ||
                              snap.last_activation_reason ==
                                  glifistore::MaintenanceActivationReason::free_space_pressure);
                GLIFI_REQUIRE(snap.last_eval_duration_ns > 0);
                GLIFI_REQUIRE((**store).close().has_value());
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("unchanged no-gain candidate suppresses rescan until change or pressure") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 0;
    config.max_no_gain_attempts = 1;

    glifistore::MaintenanceController controller{config};
    auto segment_count = std::make_shared<std::atomic<std::size_t>>(10);
    auto live_bytes = std::make_shared<std::atomic<std::uint64_t>>(1'000);
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe([segment_count, live_bytes](glifistore::MaintenanceObserveRequest)
                                -> glifistore::Result<glifistore::MaintenanceObservation> {
        const auto live = live_bytes->load(std::memory_order_relaxed);
        return glifistore::MaintenanceObservation{
            .durable = true,
            .segment_count = segment_count->load(std::memory_order_relaxed),
            .sealed_segment_count = 2,
            .compaction_candidate_worker = 0,
            .candidate_sealed_record_bytes = 2'000,
            .candidate_live_record_bytes = live,
            .candidate_dead_record_bytes = 2'000 - live,
            .candidate_dead_byte_ratio_bp = static_cast<std::uint32_t>(((2'000 - live) * 10'000U) / 2'000U),
            .max_segment_count = 100,
            .reserved_free_bytes = 1'024,
            .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
        };
    });
    controller.bind_compact(
        [compact_calls](std::optional<std::size_t>,
                        std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            compact_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::CompactionResult{
                .compacted = false,
                .worker_index = 0,
                .source_records_verified = 8,
                .source_bytes_verified = 1'000,
            };
        });
    controller.start();

    const auto wait_for_calls = [&](const std::uint64_t expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline &&
               compact_calls->load(std::memory_order_relaxed) < expected) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) >= expected);
    };
    wait_for_calls(1);

    controller.request_evaluate();
    const auto suppressed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < suppressed_deadline &&
           controller.snapshot().no_gain_scans_suppressed == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    auto snapshot = controller.snapshot();
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(snapshot.no_gain_scans_suppressed >= 1);
    GLIFI_REQUIRE(snapshot.no_gain_retry_after_ns > 0);
    GLIFI_REQUIRE(snapshot.last_skip_reason == glifistore::MaintenanceSkipReason::budget);

    live_bytes->store(999, std::memory_order_relaxed);
    controller.request_evaluate();
    wait_for_calls(2);

    segment_count->store(90, std::memory_order_relaxed);
    controller.request_evaluate();
    wait_for_calls(3);
    snapshot = controller.snapshot();
    GLIFI_REQUIRE(snapshot.pressure == glifistore::MaintenancePressureLevel::pressure);
    controller.stop();
}

GLIFI_TEST("no-gain memo expires at the bounded maximum evaluation interval") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 50;
    config.max_eval_interval_ms = 250;
    config.dead_byte_ratio_bp_normal = 0;
    config.max_no_gain_attempts = 1;

    glifistore::MaintenanceController controller{config};
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 10,
                .sealed_segment_count = 2,
                .compaction_candidate_worker = 0,
                .candidate_sealed_record_bytes = 2'000,
                .candidate_live_record_bytes = 1'000,
                .candidate_dead_record_bytes = 1'000,
                .candidate_dead_byte_ratio_bp = 5'000,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
            };
        });
    controller.bind_compact(
        [compact_calls](std::optional<std::size_t>,
                        std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            compact_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::CompactionResult{.compacted = false, .worker_index = 0};
        });
    controller.start();

    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < first_deadline &&
           compact_calls->load(std::memory_order_relaxed) < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);
    controller.request_evaluate();
    const auto suppressed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < suppressed_deadline &&
           controller.snapshot().no_gain_scans_suppressed == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    GLIFI_REQUIRE(controller.snapshot().no_gain_scans_suppressed >= 1);

    const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < retry_deadline &&
           compact_calls->load(std::memory_order_relaxed) < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) >= 2);
    controller.stop();
}

GLIFI_TEST("pressure observes no_candidate when durable sealed set is empty") {
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
                .segment_count = 90,
                .sealed_segment_count = 0,
                .max_segment_count = 100,
                .reserved_free_bytes = 100,
                .available_free_bytes = 100ULL + glifistore::kSegmentSizeBytes,
            };
        });
    controller->request_evaluate();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snap = (**store).maintenance_snapshot();
        if (snap.last_skip_reason == glifistore::MaintenanceSkipReason::no_candidate) {
            GLIFI_REQUIRE(snap.pressure == glifistore::MaintenancePressureLevel::pressure);
            GLIFI_REQUIRE(!snap.mutations_rejected);
            GLIFI_REQUIRE(snap.compact_attempts == 0 ||
                          snap.last_activation_reason ==
                              glifistore::MaintenanceActivationReason::no_candidate);
            GLIFI_REQUIRE((**store).close().has_value());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("background start evaluates promptly without request_evaluate") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 60'000;
    config.maintenance.max_eval_interval_ms = 60'000;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if ((**store).maintenance_snapshot().evaluation_cycles > 0) {
            GLIFI_REQUIRE((**store).close().has_value());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("join after stop leaves mutations_rejected cleared") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 1;
    config.maintenance.max_eval_interval_ms = 1;

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

    for (int i = 0; i < 20; ++i) {
        controller->request_evaluate();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    GLIFI_REQUIRE((**store).close().has_value());
    GLIFI_REQUIRE(!(**store).maintenance_snapshot().mutations_rejected);
    GLIFI_REQUIRE(!(**store).maintenance_snapshot().thread_running);
}

GLIFI_TEST("maintenance snapshot records expired_records_dropped from compact") {
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

    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 10,
                .sealed_segment_count = 2,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
            };
        });
    controller->bind_compact(
        [](std::optional<std::size_t>, std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            return glifistore::CompactionResult{
                .compacted = true,
                .worker_index = 0,
                .source_records_verified = 3,
                .records_copied = 2,
                .bytes_copied = 4'096,
                .expired_records_dropped = 7,
            };
        });
    controller->request_evaluate();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snap = (**store).maintenance_snapshot();
        if (snap.useful_compactions > initial.useful_compactions) {
            GLIFI_REQUIRE(snap.last_expired_records_dropped == 7);
            GLIFI_REQUIRE(snap.total_expired_records_dropped == initial.total_expired_records_dropped + 7);
            GLIFI_REQUIRE(snap.last_records_copied == 2);
            GLIFI_REQUIRE((**store).close().has_value());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("maintenance snapshot records no-gain planning scan counters") {
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

    controller->bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 10,
                .sealed_segment_count = 2,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
            };
        });
    controller->bind_compact(
        [](std::optional<std::size_t>, std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            return glifistore::CompactionResult{
                .compacted = false,
                .worker_index = 0,
                .source_records_verified = 11,
                .source_bytes_verified = 22'016,
                .expired_records_dropped = 3,
            };
        });
    controller->request_evaluate();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snap = (**store).maintenance_snapshot();
        if (snap.last_skip_reason == glifistore::MaintenanceSkipReason::no_gain &&
            snap.total_no_gain_source_records_verified > initial.total_no_gain_source_records_verified) {
            GLIFI_REQUIRE(snap.last_no_gain_source_records_verified == 11);
            GLIFI_REQUIRE(snap.last_no_gain_source_bytes_verified == 22'016);
            GLIFI_REQUIRE(snap.last_no_gain_expired_records_dropped == 3);
            GLIFI_REQUIRE(snap.total_no_gain_source_records_verified ==
                          initial.total_no_gain_source_records_verified + 11);
            GLIFI_REQUIRE(snap.total_no_gain_source_bytes_verified ==
                          initial.total_no_gain_source_bytes_verified + 22'016);
            GLIFI_REQUIRE(snap.total_no_gain_expired_records_dropped ==
                          initial.total_no_gain_expired_records_dropped + 3);
            GLIFI_REQUIRE((**store).close().has_value());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("pressure evaluation requests unread TTL probe when enabled") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 10'000;
    config.unread_ttl_pressure_probe = true;

    glifistore::MaintenanceController controller{config};
    auto probe_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe([probe_calls](glifistore::MaintenanceObserveRequest request)
                                -> glifistore::Result<glifistore::MaintenanceObservation> {
        if (request.probe_unread_expired_ttl) {
            probe_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 90,
                .sealed_segment_count = 2,
                .compaction_candidate_worker = 0,
                .candidate_sealed_record_bytes = 2'000,
                .candidate_live_record_bytes = 1'000,
                .candidate_dead_record_bytes = 1'000,
                .candidate_dead_byte_ratio_bp = 5'000,
                .unread_ttl_probe_performed = true,
                .candidate_unread_expired_sealed_record_count = 2,
                .candidate_unread_expired_sealed_record_bytes = 128,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
            };
        }
        return glifistore::MaintenanceObservation{
            .durable = true,
            .segment_count = 90,
            .sealed_segment_count = 2,
            .compaction_candidate_worker = 0,
            .candidate_sealed_record_bytes = 2'000,
            .candidate_live_record_bytes = 1'000,
            .candidate_dead_record_bytes = 1'000,
            .candidate_dead_byte_ratio_bp = 5'000,
            .max_segment_count = 100,
            .reserved_free_bytes = 1'024,
            .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
        };
    });
    controller.bind_compact([](const std::optional<std::size_t>,
                               const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
        return glifistore::CompactionResult{};
    });
    controller.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = controller.snapshot();
        if (snapshot.last_observation.unread_ttl_probe_performed &&
            snapshot.last_observation.candidate_unread_expired_sealed_record_count == 2) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(probe_calls->load(std::memory_order_relaxed) >= 1);
    const auto snapshot = controller.snapshot();
    GLIFI_REQUIRE(snapshot.last_observation.unread_ttl_probe_performed);
    GLIFI_REQUIRE(snapshot.last_observation.candidate_unread_expired_sealed_record_count == 2);
    GLIFI_REQUIRE(snapshot.last_observation.candidate_unread_expired_sealed_record_bytes == 128);
    controller.stop();
}

GLIFI_TEST("normal evaluation skips unread TTL probe") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 0;
    config.unread_ttl_pressure_probe = true;

    glifistore::MaintenanceController controller{config};
    auto probe_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe([probe_calls](glifistore::MaintenanceObserveRequest request)
                                -> glifistore::Result<glifistore::MaintenanceObservation> {
        if (request.probe_unread_expired_ttl) {
            probe_calls->fetch_add(1, std::memory_order_relaxed);
        }
        return glifistore::MaintenanceObservation{
            .durable = true,
            .segment_count = 10,
            .sealed_segment_count = 2,
            .compaction_candidate_worker = 0,
            .candidate_sealed_record_bytes = 2'000,
            .candidate_live_record_bytes = 1'000,
            .candidate_dead_record_bytes = 1'000,
            .candidate_dead_byte_ratio_bp = 5'000,
            .unread_ttl_probe_performed = request.probe_unread_expired_ttl,
            .max_segment_count = 100,
            .reserved_free_bytes = 1'024,
            .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
        };
    });
    controller.bind_compact([](const std::optional<std::size_t>,
                               const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
        return glifistore::CompactionResult{.compacted = true, .bytes_copied = 1};
    });
    controller.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (controller.snapshot().compact_completed > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(probe_calls->load(std::memory_order_relaxed) == 0);
    GLIFI_REQUIRE(!controller.snapshot().last_observation.unread_ttl_probe_performed);
    controller.stop();
}

GLIFI_TEST("normal unread TTL scheduling probes and lowers reclaim threshold") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 5'000;
    config.unread_ttl_normal_scheduling = true;

    glifistore::MaintenanceController controller{config};
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe([](glifistore::MaintenanceObserveRequest request)
                                -> glifistore::Result<glifistore::MaintenanceObservation> {
        glifistore::MaintenanceObservation observation{
            .durable = true,
            .segment_count = 10,
            .sealed_segment_count = 2,
            .compaction_candidate_worker = 0,
            .candidate_sealed_record_bytes = 10'000,
            .candidate_live_record_bytes = 6'500,
            .candidate_dead_record_bytes = 3'500,
            .candidate_dead_byte_ratio_bp = 3'500,
            .max_segment_count = 100,
            .reserved_free_bytes = 1'024,
            .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
        };
        if (request.probe_unread_expired_ttl) {
            observation.unread_ttl_probe_performed = true;
            observation.candidate_unread_expired_sealed_record_count = 1;
            observation.candidate_unread_expired_sealed_record_bytes = 2'000;
        }
        observation.candidate_scheduling_dead_byte_ratio_bp =
            glifistore::scheduling_dead_byte_ratio_bp(observation);
        return observation;
    });
    controller.bind_compact(
        [compact_calls](const std::optional<std::size_t>,
                        const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            compact_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::CompactionResult{.compacted = true, .bytes_copied = 1};
        });
    controller.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = controller.snapshot();
        if (snapshot.compact_completed > 0) {
            GLIFI_REQUIRE(snapshot.last_observation.unread_ttl_probe_performed);
            GLIFI_REQUIRE(snapshot.last_observation.candidate_scheduling_dead_byte_ratio_bp == 5'500);
            GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) >= 1);
            controller.stop();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("normal unread TTL scheduling disabled keeps conservative threshold") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 5'000;
    config.unread_ttl_normal_scheduling = false;

    glifistore::MaintenanceController controller{config};
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe([](glifistore::MaintenanceObserveRequest request)
                                -> glifistore::Result<glifistore::MaintenanceObservation> {
        if (request.probe_unread_expired_ttl) {
            return glifistore::fail(glifistore::ErrorCode::internal_error, "unexpected probe");
        }
        return glifistore::MaintenanceObservation{
            .durable = true,
            .segment_count = 10,
            .sealed_segment_count = 2,
            .compaction_candidate_worker = 0,
            .candidate_sealed_record_bytes = 10'000,
            .candidate_live_record_bytes = 6'500,
            .candidate_dead_record_bytes = 3'500,
            .candidate_dead_byte_ratio_bp = 3'500,
            .candidate_scheduling_dead_byte_ratio_bp = 3'500,
            .max_segment_count = 100,
            .reserved_free_bytes = 1'024,
            .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
        };
    });
    controller.bind_compact(
        [compact_calls](const std::optional<std::size_t>,
                        const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            compact_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::CompactionResult{.compacted = true, .bytes_copied = 1};
        });
    controller.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = controller.snapshot();
        if (snapshot.skips > 0 &&
            snapshot.last_skip_reason == glifistore::MaintenanceSkipReason::reclaim_threshold) {
            GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 0);
            controller.stop();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(false);
}

GLIFI_TEST("max_copy_bytes_per_sec budget refreshes after the one-second window") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 0;
    config.max_copy_bytes_per_cycle = 0;
    config.max_copy_bytes_per_sec = 1'000;

    glifistore::MaintenanceController controller{config};
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 10,
                .sealed_segment_count = 2,
                .compaction_candidate_worker = 0,
                .candidate_sealed_record_bytes = 2'000,
                .candidate_live_record_bytes = 1'000,
                .candidate_dead_record_bytes = 1'000,
                .candidate_dead_byte_ratio_bp = 5'000,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
            };
        });
    controller.bind_compact(
        [compact_calls](const std::optional<std::size_t>,
                        const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            compact_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::CompactionResult{.compacted = true, .bytes_copied = 1'000};
        });
    controller.start();

    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < first_deadline &&
           compact_calls->load(std::memory_order_relaxed) < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);

    controller.request_evaluate();
    const auto skip_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < skip_deadline &&
           controller.snapshot().last_skip_reason != glifistore::MaintenanceSkipReason::rate_budget) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(controller.snapshot().last_skip_reason == glifistore::MaintenanceSkipReason::rate_budget);
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds{1'100});
    controller.request_evaluate();
    const auto second_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < second_deadline &&
           compact_calls->load(std::memory_order_relaxed) < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 2);
    controller.stop();
}
