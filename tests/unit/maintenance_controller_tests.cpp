#include "glifistore/core/integer_math.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/store/maintenance.hpp"
#include "glifistore/store/store.hpp"
#include "maintenance_controller_test_support.hpp"
#include "store/store_internal.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>

GLIFI_TEST("validate maintenance config rejects inverted intervals") {
    glifistore::MaintenanceConfig config{};
    config.min_eval_interval_ms = 5'000;
    config.max_eval_interval_ms = 1'000;
    const auto status = glifistore::validate_maintenance_config(config);
    GLIFI_REQUIRE(!status.has_value());
    GLIFI_REQUIRE(status.error().code == glifistore::ErrorCode::invalid_argument);

    config.min_eval_interval_ms = 1'000;
    config.suspend_on_p99_min_samples = 0;
    const auto samples = glifistore::validate_maintenance_config(config);
    GLIFI_REQUIRE(!samples.has_value());
    GLIFI_REQUIRE(samples.error().code == glifistore::ErrorCode::invalid_argument);

    config.suspend_on_p99_min_samples = 32;
    config.max_segments_per_cycle = 2;
    const auto v1_transaction_count = glifistore::validate_maintenance_config(config);
    GLIFI_REQUIRE(!v1_transaction_count.has_value());
    GLIFI_REQUIRE(v1_transaction_count.error().code == glifistore::ErrorCode::invalid_argument);
}

GLIFI_TEST("maintenance integer ratios and pressure thresholds do not overflow") {
    constexpr auto maximum_u64 = std::numeric_limits<std::uint64_t>::max();
    constexpr auto maximum_size = std::numeric_limits<std::size_t>::max();
    GLIFI_REQUIRE(glifistore::basis_points(maximum_u64 / 2U, maximum_u64) == 4'999);
    GLIFI_REQUIRE(glifistore::basis_points(maximum_u64 - 1U, maximum_u64) == 9'999);
    GLIFI_REQUIRE(glifistore::basis_points(maximum_u64, maximum_u64) == 10'000);
    for (std::uint64_t denominator = 1; denominator <= 1'000; ++denominator) {
        for (std::uint64_t numerator = 0; numerator <= denominator; ++numerator) {
            GLIFI_REQUIRE(glifistore::basis_points(numerator, denominator) ==
                          numerator * 10'000U / denominator);
        }
    }

    glifistore::MaintenanceConfig config{};
    config.segment_count_pressure_pct = 100;
    auto observation = glifistore::MaintenanceObservation{
        .durable = true,
        .segment_count = maximum_size - 1U,
        .max_segment_count = maximum_size,
    };
    GLIFI_REQUIRE(glifistore::classify_maintenance_pressure(observation, config) ==
                  glifistore::MaintenancePressureLevel::normal);

    observation.segment_count = 1;
    observation.reserved_free_bytes = maximum_u64 - 100U;
    observation.rotate_additional_bytes = 50;
    observation.available_free_bytes = maximum_u64 - 25U;
    config.free_bytes_pressure_margin = 200;
    GLIFI_REQUIRE(glifistore::classify_maintenance_pressure(observation, config) ==
                  glifistore::MaintenancePressureLevel::pressure);

    observation.candidate_dead_byte_ratio_bp = 9'999;
    observation.candidate_sealed_record_bytes = maximum_u64;
    observation.candidate_dead_record_bytes = maximum_u64 - 100U;
    observation.candidate_unread_expired_sealed_record_bytes = 200;
    GLIFI_REQUIRE(glifistore::scheduling_dead_byte_ratio_bp(observation) == 10'000);
}

GLIFI_TEST("reclaim_threshold skip advances to a reclaimable peer Worker") {
    // HAZ-026: a below-threshold Worker must not pin evaluation when observe advances the
    // round-robin cursor (Store advances on every observation, including reclaim_threshold skips).
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 5'000;

    glifistore::MaintenanceController controller{config};
    auto observe_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto selected_worker =
        std::make_shared<std::atomic<std::size_t>>(std::numeric_limits<std::size_t>::max());
    controller.bind_observe([observe_calls](glifistore::MaintenanceObserveRequest)
                                -> glifistore::Result<glifistore::MaintenanceObservation> {
        const auto call = observe_calls->fetch_add(1, std::memory_order_relaxed);
        if (call == 0) {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 10,
                .sealed_segment_count = 2,
                .compaction_candidate_worker = 0,
                .candidate_sealed_record_bytes = 10'000,
                .candidate_live_record_bytes = 10'000,
                .candidate_dead_record_bytes = 0,
                .candidate_dead_byte_ratio_bp = 0,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
            };
        }
        return glifistore::MaintenanceObservation{
            .durable = true,
            .segment_count = 10,
            .sealed_segment_count = 2,
            .compaction_candidate_worker = 1,
            .candidate_sealed_record_bytes = 10'000,
            .candidate_live_record_bytes = 4'000,
            .candidate_dead_record_bytes = 6'000,
            .candidate_dead_byte_ratio_bp = 6'000,
            .max_segment_count = 100,
            .reserved_free_bytes = 1'024,
            .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
        };
    });
    controller.bind_compact([compact_calls, selected_worker](
                                const std::optional<std::size_t> worker,
                                const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
        compact_calls->fetch_add(1, std::memory_order_relaxed);
        selected_worker->store(worker.value_or(std::numeric_limits<std::size_t>::max()),
                               std::memory_order_relaxed);
        return glifistore::CompactionResult{.compacted = true, .bytes_copied = 1};
    });
    controller.start();

    const auto skip_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < skip_deadline) {
        const auto snapshot = controller.snapshot();
        if (snapshot.last_skip_reason == glifistore::MaintenanceSkipReason::reclaim_threshold) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(controller.snapshot().last_skip_reason ==
                  glifistore::MaintenanceSkipReason::reclaim_threshold);
    GLIFI_REQUIRE(controller.snapshot().last_observation.compaction_candidate_worker == 0);
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 0);

    controller.request_evaluate();
    const auto compact_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < compact_deadline &&
           compact_calls->load(std::memory_order_relaxed) < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(selected_worker->load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(controller.snapshot().useful_compactions >= 1);
    controller.stop();
}

GLIFI_TEST("normal dead-byte threshold is inclusive and pressure bypasses it") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 5'000;

    glifistore::MaintenanceController controller{config};
    auto ratio = std::make_shared<std::atomic<std::uint32_t>>(4'999);
    auto segment_count = std::make_shared<std::atomic<std::size_t>>(10);
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto selected_worker =
        std::make_shared<std::atomic<std::size_t>>(std::numeric_limits<std::size_t>::max());
    controller.bind_observe([ratio, segment_count](glifistore::MaintenanceObserveRequest)
                                -> glifistore::Result<glifistore::MaintenanceObservation> {
        return glifistore::MaintenanceObservation{
            .durable = true,
            .segment_count = segment_count->load(std::memory_order_relaxed),
            .sealed_segment_count = 2,
            .compaction_candidate_worker = 3,
            .candidate_sealed_record_bytes = 10'000,
            .candidate_live_record_bytes = 10'000 - ratio->load(std::memory_order_relaxed),
            .candidate_dead_record_bytes = ratio->load(std::memory_order_relaxed),
            .candidate_dead_byte_ratio_bp = ratio->load(std::memory_order_relaxed),
            .max_segment_count = 100,
            .reserved_free_bytes = 1'024,
            .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
        };
    });
    controller.bind_compact([compact_calls, selected_worker](
                                const std::optional<std::size_t> worker,
                                const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
        compact_calls->fetch_add(1, std::memory_order_relaxed);
        selected_worker->store(worker.value_or(std::numeric_limits<std::size_t>::max()),
                               std::memory_order_relaxed);
        return glifistore::CompactionResult{};
    });
    controller.start();

    const auto below_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < below_deadline) {
        const auto snapshot = controller.snapshot();
        if (snapshot.last_skip_reason == glifistore::MaintenanceSkipReason::reclaim_threshold) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(controller.snapshot().last_skip_reason ==
                  glifistore::MaintenanceSkipReason::reclaim_threshold);
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 0);

    ratio->store(5'000, std::memory_order_relaxed);
    controller.request_evaluate();
    const auto threshold_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < threshold_deadline &&
           compact_calls->load(std::memory_order_relaxed) < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(selected_worker->load(std::memory_order_relaxed) == 3);

    ratio->store(0, std::memory_order_relaxed);
    segment_count->store(90, std::memory_order_relaxed);
    controller.request_evaluate();
    const auto pressure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < pressure_deadline &&
           compact_calls->load(std::memory_order_relaxed) < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto pressure_snapshot = controller.snapshot();
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 2);
    GLIFI_REQUIRE(pressure_snapshot.pressure == glifistore::MaintenancePressureLevel::pressure);
    controller.stop();
}

GLIFI_TEST("normal copy budget bounds one prefix transaction and pressure bypasses it") {
    glifistore::MaintenanceConfig config{};
    GLIFI_REQUIRE(config.max_copy_bytes_per_cycle == glifistore::kDefaultMaintenanceMaxCopyBytesPerCycle);
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 0;
    config.max_copy_bytes_per_cycle = 1'000;

    glifistore::MaintenanceController controller{config};
    auto candidate_live_bytes = std::make_shared<std::atomic<std::uint64_t>>(1'001);
    auto segment_count = std::make_shared<std::atomic<std::size_t>>(10);
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto observed_copy_limit = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe([candidate_live_bytes, segment_count](glifistore::MaintenanceObserveRequest)
                                -> glifistore::Result<glifistore::MaintenanceObservation> {
        return glifistore::MaintenanceObservation{
            .durable = true,
            .segment_count = segment_count->load(std::memory_order_relaxed),
            .sealed_segment_count = 2,
            .compaction_candidate_worker = 0,
            .candidate_sealed_record_bytes = 2'000,
            .candidate_live_record_bytes = candidate_live_bytes->load(std::memory_order_relaxed),
            .candidate_dead_record_bytes = 999,
            .candidate_dead_byte_ratio_bp = 5'000,
            .max_segment_count = 100,
            .reserved_free_bytes = 1'024,
            .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
        };
    });
    controller.bind_compact([compact_calls, observed_copy_limit](const std::optional<std::size_t>,
                                                                 const std::uint64_t max_copy_bytes)
                                -> glifistore::Result<glifistore::CompactionResult> {
        observed_copy_limit->store(max_copy_bytes, std::memory_order_relaxed);
        compact_calls->fetch_add(1, std::memory_order_release);
        // A useful prefix transaction: no-gain backoff must not hide later calls.
        return glifistore::CompactionResult{.compacted = true, .worker_index = 0};
    });
    controller.start();

    // ADR 0044: a candidate above the per-cycle budget is no longer skipped;
    // the runtime receives the budget and compacts the oldest prefix that fits.
    const auto over_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < over_deadline &&
           compact_calls->load(std::memory_order_acquire) < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_acquire) == 1);
    GLIFI_REQUIRE(observed_copy_limit->load(std::memory_order_relaxed) == 1'000);
    GLIFI_REQUIRE(controller.snapshot().last_skip_reason != glifistore::MaintenanceSkipReason::copy_budget);

    candidate_live_bytes->store(1'000, std::memory_order_relaxed);
    controller.request_evaluate();
    const auto equal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < equal_deadline &&
           compact_calls->load(std::memory_order_acquire) < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_acquire) == 2);
    GLIFI_REQUIRE(observed_copy_limit->load(std::memory_order_relaxed) == 1'000);

    candidate_live_bytes->store(1'001, std::memory_order_relaxed);
    segment_count->store(90, std::memory_order_relaxed);
    controller.request_evaluate();
    const auto pressure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < pressure_deadline &&
           compact_calls->load(std::memory_order_acquire) < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_acquire) == 3);
    GLIFI_REQUIRE(observed_copy_limit->load(std::memory_order_relaxed) == 0);
    GLIFI_REQUIRE(controller.snapshot().pressure == glifistore::MaintenancePressureLevel::pressure);
    controller.stop();

    config.max_copy_bytes_per_cycle = 0;
    segment_count->store(10, std::memory_order_relaxed);
    compact_calls->store(0, std::memory_order_relaxed);
    glifistore::MaintenanceController unlimited_controller{config};
    unlimited_controller.bind_observe(
        [candidate_live_bytes, segment_count](
            glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = segment_count->load(std::memory_order_relaxed),
                .sealed_segment_count = 2,
                .compaction_candidate_worker = 0,
                .candidate_sealed_record_bytes = 2'000,
                .candidate_live_record_bytes = candidate_live_bytes->load(std::memory_order_relaxed),
                .candidate_dead_record_bytes = 999,
                .candidate_dead_byte_ratio_bp = 5'000,
                .max_segment_count = 100,
                .reserved_free_bytes = 1'024,
                .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
            };
        });
    unlimited_controller.bind_compact(
        [compact_calls](const std::optional<std::size_t>,
                        const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            compact_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::CompactionResult{};
        });
    unlimited_controller.start();
    const auto unlimited_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < unlimited_deadline &&
           compact_calls->load(std::memory_order_relaxed) < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);
    unlimited_controller.stop();
}

GLIFI_TEST("normal rate and cpu budgets suspend and pressure bypasses them") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 0;
    config.max_copy_bytes_per_cycle = 0;
    config.max_copy_bytes_per_sec = 500;
    config.max_cpu_ms_per_window = 0;

    glifistore::MaintenanceController controller{config};
    auto segment_count = std::make_shared<std::atomic<std::size_t>>(10);
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto observed_copy_limit = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe([segment_count](glifistore::MaintenanceObserveRequest)
                                -> glifistore::Result<glifistore::MaintenanceObservation> {
        return glifistore::MaintenanceObservation{
            .durable = true,
            .segment_count = segment_count->load(std::memory_order_relaxed),
            .sealed_segment_count = 2,
            .compaction_candidate_worker = 0,
            .candidate_sealed_record_bytes = 2'000,
            .candidate_live_record_bytes = 600,
            .candidate_dead_record_bytes = 1'400,
            .candidate_dead_byte_ratio_bp = 8'000,
            .max_segment_count = 100,
            .reserved_free_bytes = 1'024,
            .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
        };
    });
    controller.bind_compact([compact_calls, observed_copy_limit](const std::optional<std::size_t>,
                                                                 const std::uint64_t max_copy_bytes)
                                -> glifistore::Result<glifistore::CompactionResult> {
        observed_copy_limit->store(max_copy_bytes, std::memory_order_relaxed);
        compact_calls->fetch_add(1, std::memory_order_release);
        return glifistore::CompactionResult{
            .compacted = true,
            .records_copied = 1,
            .bytes_copied = 600,
        };
    });
    controller.start();

    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < first_deadline &&
           controller.snapshot().rate_window_bytes_copied != 600) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_acquire) == 1);
    // max_copy_bytes_per_sec is pacing, not a whole-transaction cap. The
    // independent per-cycle limit is disabled, so a candidate larger than one
    // second of bandwidth must still be admitted.
    GLIFI_REQUIRE(observed_copy_limit->load(std::memory_order_relaxed) == 0);
    GLIFI_REQUIRE(controller.snapshot().rate_window_bytes_copied == 600);

    controller.request_evaluate();
    const auto rate_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < rate_deadline &&
           controller.snapshot().last_skip_reason != glifistore::MaintenanceSkipReason::rate_budget) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(controller.snapshot().last_skip_reason == glifistore::MaintenanceSkipReason::rate_budget);
    GLIFI_REQUIRE(controller.snapshot().last_activation_reason ==
                  glifistore::MaintenanceActivationReason::rate_budget);
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);

    segment_count->store(90, std::memory_order_relaxed);
    controller.request_evaluate();
    const auto pressure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < pressure_deadline &&
           compact_calls->load(std::memory_order_acquire) < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_acquire) == 2);
    GLIFI_REQUIRE(observed_copy_limit->load(std::memory_order_relaxed) == 0);
    GLIFI_REQUIRE(controller.snapshot().pressure == glifistore::MaintenancePressureLevel::pressure);
    controller.stop();

    config.max_copy_bytes_per_sec = 0;
    config.max_cpu_ms_per_window = 1;
    segment_count->store(10, std::memory_order_relaxed);
    compact_calls->store(0, std::memory_order_relaxed);
    glifistore::MaintenanceController cpu_controller{config};
    cpu_controller.bind_observe([segment_count](glifistore::MaintenanceObserveRequest)
                                    -> glifistore::Result<glifistore::MaintenanceObservation> {
        return glifistore::MaintenanceObservation{
            .durable = true,
            .segment_count = segment_count->load(std::memory_order_relaxed),
            .sealed_segment_count = 2,
            .compaction_candidate_worker = 0,
            .candidate_sealed_record_bytes = 2'000,
            .candidate_live_record_bytes = 600,
            .candidate_dead_record_bytes = 1'400,
            .candidate_dead_byte_ratio_bp = 8'000,
            .max_segment_count = 100,
            .reserved_free_bytes = 1'024,
            .available_free_bytes = 1'024ULL + glifistore::kSegmentSizeBytes + 4'096ULL,
        };
    });
    cpu_controller.bind_compact(
        [compact_calls](const std::optional<std::size_t>,
                        const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
            }
            compact_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::CompactionResult{
                .compacted = true,
                .records_copied = 1,
                .bytes_copied = 10,
            };
        });
    cpu_controller.start();
    const auto cpu_first = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < cpu_first &&
           cpu_controller.snapshot().rate_window_cpu_ns < 1'000'000ULL) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);
    const auto after_cpu = cpu_controller.snapshot();
    GLIFI_REQUIRE(after_cpu.last_compact_duration_ns >= 1'000'000ULL);
    GLIFI_REQUIRE(after_cpu.rate_window_cpu_ns >= 1'000'000ULL);
    cpu_controller.request_evaluate();
    const auto cpu_budget_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < cpu_budget_deadline &&
           cpu_controller.snapshot().last_skip_reason != glifistore::MaintenanceSkipReason::rate_budget) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(cpu_controller.snapshot().last_skip_reason ==
                  glifistore::MaintenanceSkipReason::rate_budget);
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);
    cpu_controller.stop();
}

GLIFI_TEST("foreground p99 suspends normal compaction and pressure bypasses latency fairness") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 0;
    config.suspend_on_p99_latency_ms = 25;
    config.suspend_on_p99_min_samples = 1;

    glifistore::MaintenanceController controller{config};
    auto segment_count = std::make_shared<std::atomic<std::size_t>>(10);
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe([segment_count](glifistore::MaintenanceObserveRequest)
                                -> glifistore::Result<glifistore::MaintenanceObservation> {
        return glifistore::MaintenanceObservation{
            .durable = true,
            .segment_count = segment_count->load(std::memory_order_relaxed),
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
            return glifistore::CompactionResult{};
        });

    controller.report_foreground_latency(40'000'000ULL);
    controller.start();
    const auto suspended_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < suspended_deadline &&
           controller.snapshot().last_skip_reason != glifistore::MaintenanceSkipReason::latency_budget) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto suspended = controller.snapshot();
    GLIFI_REQUIRE(suspended.last_skip_reason == glifistore::MaintenanceSkipReason::latency_budget);
    GLIFI_REQUIRE(suspended.last_activation_reason ==
                  glifistore::MaintenanceActivationReason::latency_budget);
    GLIFI_REQUIRE(suspended.foreground_latency_samples == 1);
    GLIFI_REQUIRE(suspended.last_foreground_p99_ns == 50'000'000ULL);
    GLIFI_REQUIRE(suspended.latency_suspends == 1);
    GLIFI_REQUIRE(suspended.latency_guard_active);
    GLIFI_REQUIRE(suspended.latency_deferral_age_ns > 0);
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 0);

    segment_count->store(90, std::memory_order_relaxed);
    controller.report_foreground_latency(40'000'000ULL);
    controller.request_evaluate();
    const auto pressure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < pressure_deadline &&
           compact_calls->load(std::memory_order_relaxed) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto pressure = controller.snapshot();
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(pressure.pressure == glifistore::MaintenancePressureLevel::pressure);
    GLIFI_REQUIRE(pressure.foreground_latency_samples == 1);
    GLIFI_REQUIRE(pressure.latency_suspends == 1);
    GLIFI_REQUIRE(!pressure.latency_guard_active);
    controller.stop();
}

GLIFI_TEST("foreground latency guard requires a representative sample window") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 0;
    config.suspend_on_p99_latency_ms = 25;
    config.suspend_on_p99_min_samples = 2;

    glifistore::MaintenanceController controller{config};
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 3,
                .sealed_segment_count = 2,
                .compaction_candidate_worker = 0,
                .candidate_sealed_record_bytes = 2'000,
                .candidate_live_record_bytes = 1'000,
                .candidate_dead_record_bytes = 1'000,
                .candidate_dead_byte_ratio_bp = 5'000,
                .max_segment_count = 100,
            };
        });
    controller.bind_compact(
        [compact_calls](const std::optional<std::size_t>,
                        const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            compact_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::CompactionResult{};
        });
    controller.report_foreground_latency(40'000'000ULL);
    controller.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline &&
           compact_calls->load(std::memory_order_relaxed) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto snapshot = controller.snapshot();
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(snapshot.foreground_latency_samples == 1);
    GLIFI_REQUIRE(!snapshot.latency_guard_active);
    GLIFI_REQUIRE(snapshot.latency_suspends == 0);
    controller.stop();
}

GLIFI_TEST("foreground latency guard uses hysteresis and bounded reclaim debt") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;
    config.dead_byte_ratio_bp_normal = 0;
    config.suspend_on_p99_latency_ms = 25;
    config.suspend_on_p99_min_samples = 1;
    // Headroom above CI scheduling jitter so hysteresis checks are not raced by debt override.
    config.max_latency_deferral_ms = 500;
    // This test isolates latency-state transitions; do not let its synthetic
    // unchanged no-gain callback engage the independent no-gain memo.
    config.max_no_gain_attempts = 0;

    glifistore::MaintenanceController controller{config};
    auto compact_calls = std::make_shared<std::atomic<std::uint64_t>>(0);
    controller.bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 3,
                .sealed_segment_count = 2,
                .compaction_candidate_worker = 0,
                .candidate_sealed_record_bytes = 2'000,
                .candidate_live_record_bytes = 1'000,
                .candidate_dead_record_bytes = 1'000,
                .candidate_dead_byte_ratio_bp = 5'000,
                .max_segment_count = 100,
            };
        });
    controller.bind_compact(
        [compact_calls](const std::optional<std::size_t>,
                        const std::uint64_t) -> glifistore::Result<glifistore::CompactionResult> {
            compact_calls->fetch_add(1, std::memory_order_relaxed);
            return glifistore::CompactionResult{};
        });

    controller.report_foreground_latency(40'000'000ULL);
    controller.start();
    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < first_deadline && controller.snapshot().latency_suspends < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(controller.snapshot().latency_guard_active);

    // A conservative 25 ms bucket stays latched until p99 falls below the
    // derived 20 ms resume boundary.
    controller.report_foreground_latency(25'000'000ULL);
    controller.request_evaluate();
    const auto second_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < second_deadline && controller.snapshot().latency_suspends < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(controller.snapshot().latency_guard_active);
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 0);

    controller.report_foreground_latency(10'000'000ULL);
    controller.request_evaluate();
    const auto resume_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < resume_deadline &&
           compact_calls->load(std::memory_order_relaxed) < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 1);
    GLIFI_REQUIRE(!controller.snapshot().latency_guard_active);

    controller.report_foreground_latency(40'000'000ULL);
    controller.request_evaluate();
    const auto rearm_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < rearm_deadline && controller.snapshot().latency_suspends < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(controller.snapshot().latency_guard_active);

    std::this_thread::sleep_for(std::chrono::milliseconds{520});
    controller.report_foreground_latency(40'000'000ULL);
    controller.request_evaluate();
    const auto debt_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < debt_deadline &&
           compact_calls->load(std::memory_order_relaxed) < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto debt = controller.snapshot();
    GLIFI_REQUIRE(compact_calls->load(std::memory_order_relaxed) == 2);
    GLIFI_REQUIRE(debt.latency_debt_overrides == 1);
    GLIFI_REQUIRE(!debt.latency_guard_active);
    GLIFI_REQUIRE(debt.last_activation_reason ==
                  glifistore::MaintenanceActivationReason::latency_debt_override);
    controller.stop();
}

GLIFI_TEST("maintenance telemetry counts sequence conflicts") {
    glifistore::MaintenanceConfig config{};
    config.mode = glifistore::MaintenanceMode::background;
    config.min_eval_interval_ms = 60'000;
    config.max_eval_interval_ms = 60'000;

    glifistore::MaintenanceController controller{config};
    controller.bind_observe(
        [](glifistore::MaintenanceObserveRequest) -> glifistore::Result<glifistore::MaintenanceObservation> {
            return glifistore::MaintenanceObservation{
                .durable = true,
                .segment_count = 3,
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
        return glifistore::fail(glifistore::ErrorCode::sequence_conflict, "injected maintenance conflict");
    });
    controller.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline && controller.snapshot().sequence_conflicts == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto snapshot = controller.snapshot();
    GLIFI_REQUIRE(snapshot.compact_attempts == 1);
    GLIFI_REQUIRE(snapshot.sequence_conflicts == 1);
    GLIFI_REQUIRE(snapshot.skips == 1);
    GLIFI_REQUIRE(snapshot.last_skip_reason == glifistore::MaintenanceSkipReason::sequence_conflict);
    controller.stop();
}

GLIFI_TEST("cooperative store starts no maintenance thread") {
    auto store = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .maintenance = {.mode = glifistore::MaintenanceMode::cooperative},
    });
    GLIFI_REQUIRE(store.has_value());
    const auto snapshot = (**store).maintenance_snapshot();
    GLIFI_REQUIRE(snapshot.mode == glifistore::MaintenanceMode::cooperative);
    GLIFI_REQUIRE(!snapshot.thread_running);
    GLIFI_REQUIRE(snapshot.state == glifistore::MaintenanceState::stopped);
    GLIFI_REQUIRE((**store).close().has_value());
}

GLIFI_TEST("disabled store starts no maintenance thread") {
    auto store = glifistore::Store::open({
        .worker_config = {.explicit_count = 1},
        .maintenance = {.mode = glifistore::MaintenanceMode::disabled},
    });
    GLIFI_REQUIRE(store.has_value());
    GLIFI_REQUIRE(!(**store).maintenance_snapshot().thread_running);
    GLIFI_REQUIRE((**store).close().has_value());
}

GLIFI_TEST("background store phase1 auto-compacts and reports no_gain on empty volatile") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 60'000;
    config.maintenance.max_eval_interval_ms = 60'000;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    GLIFI_REQUIRE((**store).maintenance_snapshot().thread_running);

    const auto snapshot = wait_for_initial_idle(**store);
    GLIFI_REQUIRE(snapshot.evaluation_cycles > 0);
    GLIFI_REQUIRE(snapshot.compact_attempts > 0);
    GLIFI_REQUIRE(snapshot.last_skip_reason == glifistore::MaintenanceSkipReason::no_gain ||
                  snapshot.compact_completed > 0);
    GLIFI_REQUIRE(!snapshot.last_observation.durable);

    const auto compacted = (**store).compact();
    GLIFI_REQUIRE(compacted.has_value());

    GLIFI_REQUIRE((**store).close().has_value());
    GLIFI_REQUIRE(!(**store).maintenance_snapshot().thread_running);
}

GLIFI_TEST("background auto-compact can be disabled for observation-only eval") {
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
    controller->request_evaluate();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    glifistore::MaintenanceSnapshot snapshot{};
    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = (**store).maintenance_snapshot();
        if (snapshot.evaluation_cycles > baseline_cycles &&
            snapshot.last_skip_reason == glifistore::MaintenanceSkipReason::policy_deferred) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(snapshot.evaluation_cycles > baseline_cycles);
    GLIFI_REQUIRE(snapshot.compact_attempts == baseline_attempts);
    GLIFI_REQUIRE(snapshot.last_skip_reason == glifistore::MaintenanceSkipReason::policy_deferred);
    GLIFI_REQUIRE((**store).close().has_value());
}

GLIFI_TEST("manual compact races background controller without queuing") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 1;
    config.maintenance.max_eval_interval_ms = 1;
    config.maintenance.max_no_gain_attempts = 1'000;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);

    bool saw_conflict = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        controller->request_evaluate();
        const auto result = (**store).compact();
        if (!result.has_value() && result.error().code == glifistore::ErrorCode::sequence_conflict) {
            saw_conflict = true;
            break;
        }
    }
    GLIFI_REQUIRE(saw_conflict);
    GLIFI_REQUIRE((**store).close().has_value());
}

GLIFI_TEST("normal policy respects max_no_gain budget backoff") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 5;
    config.maintenance.max_eval_interval_ms = 60'000;
    config.maintenance.max_no_gain_attempts = 2;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    bool saw_budget = false;
    while (std::chrono::steady_clock::now() < deadline) {
        controller->request_evaluate();
        const auto snapshot = (**store).maintenance_snapshot();
        if (snapshot.last_skip_reason == glifistore::MaintenanceSkipReason::budget ||
            snapshot.state == glifistore::MaintenanceState::suspended) {
            saw_budget = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    GLIFI_REQUIRE(saw_budget);
    GLIFI_REQUIRE((**store).close().has_value());
}

GLIFI_TEST("close joins maintenance after draining admitted operations") {
    glifistore::StoreConfig config{};
    config.worker_config.explicit_count = 1;
    config.maintenance.mode = glifistore::MaintenanceMode::background;
    config.maintenance.min_eval_interval_ms = 1;
    config.maintenance.max_eval_interval_ms = 1;

    auto store = glifistore::Store::open(config);
    GLIFI_REQUIRE(store.has_value());
    auto* controller = glifistore::detail::StoreAccess::maintenance_controller(**store);
    GLIFI_REQUIRE(controller != nullptr);
    controller->request_evaluate();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    GLIFI_REQUIRE((**store).close().has_value());
    GLIFI_REQUIRE(!(**store).maintenance_snapshot().thread_running);
    GLIFI_REQUIRE((**store).maintenance_snapshot().state == glifistore::MaintenanceState::stopped);
}
