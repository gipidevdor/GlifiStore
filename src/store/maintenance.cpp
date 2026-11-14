#include "glifistore/store/maintenance.hpp"

#include "glifistore/core/integer_math.hpp"
#include "glifistore/core/types.hpp"
#include "maintenance_detail.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace glifistore {
using maintenance_detail::aggressive_pressure;
using maintenance_detail::ceil_percentage;
using maintenance_detail::clamp_interval_ms;
using maintenance_detail::elapsed_ns;
using maintenance_detail::free_space_blocks_rotation;

auto classify_maintenance_pressure(const MaintenanceObservation& observation,
                                   const MaintenanceConfig& config) noexcept -> MaintenancePressureLevel {
    if (!observation.durable) {
        return MaintenancePressureLevel::normal;
    }

    // Emergency: cannot create/rotate a Segment while preserving reserved free space.
    if (observation.max_segment_count > 0 && observation.segment_count >= observation.max_segment_count) {
        return MaintenancePressureLevel::emergency;
    }
    if (free_space_blocks_rotation(observation)) {
        return MaintenancePressureLevel::emergency;
    }

    bool pressure = false;
    if (observation.max_segment_count > 0) {
        const auto threshold =
            ceil_percentage(observation.max_segment_count, config.segment_count_pressure_pct);
        if (observation.segment_count >= threshold) {
            pressure = true;
        }
    }
    if (observation.available_free_bytes.has_value()) {
        const auto watermark =
            saturating_add(observation.reserved_free_bytes, config.free_bytes_pressure_margin);
        if (*observation.available_free_bytes <= watermark) {
            pressure = true;
        }
    }
    return pressure ? MaintenancePressureLevel::pressure : MaintenancePressureLevel::normal;
}

auto scheduling_dead_byte_ratio_bp(const MaintenanceObservation& observation) noexcept
    -> std::optional<std::uint32_t> {
    if (!observation.candidate_dead_byte_ratio_bp || observation.candidate_sealed_record_bytes == 0) {
        return observation.candidate_dead_byte_ratio_bp;
    }
    const auto effective_dead = saturating_add(observation.candidate_dead_record_bytes,
                                               observation.candidate_unread_expired_sealed_record_bytes);
    const auto capped = std::min(effective_dead, observation.candidate_sealed_record_bytes);
    return basis_points(capped, observation.candidate_sealed_record_bytes);
}

auto validate_maintenance_config(const MaintenanceConfig& config) -> Status {
    if (config.mode != MaintenanceMode::cooperative && config.mode != MaintenanceMode::background &&
        config.mode != MaintenanceMode::disabled) {
        return fail(ErrorCode::invalid_argument, "maintenance mode is unsupported");
    }
    if (config.min_eval_interval_ms == 0 || config.max_eval_interval_ms == 0) {
        return fail(ErrorCode::invalid_argument, "maintenance eval intervals must be greater than zero");
    }
    if (config.min_eval_interval_ms > config.max_eval_interval_ms) {
        return fail(ErrorCode::invalid_argument, "maintenance min_eval_interval_ms exceeds max");
    }
    if (config.max_segments_per_cycle != 1) {
        return fail(ErrorCode::invalid_argument,
                    "persistence v1 requires maintenance max_segments_per_cycle to equal one");
    }
    if (config.segment_count_pressure_pct == 0 || config.segment_count_pressure_pct > 100) {
        return fail(ErrorCode::invalid_argument, "maintenance segment_count_pressure_pct must be in 1..100");
    }
    if (config.dead_byte_ratio_bp_normal > 10'000) {
        return fail(ErrorCode::invalid_argument, "maintenance dead_byte_ratio_bp_normal must be <= 10000");
    }
    if (config.suspend_on_p99_min_samples == 0) {
        return fail(ErrorCode::invalid_argument,
                    "maintenance suspend_on_p99_min_samples must be greater than zero");
    }
    return {};
}

MaintenanceController::MaintenanceController(MaintenanceConfig config) : config_(config) {}

MaintenanceController::~MaintenanceController() {
    stop();
}

void MaintenanceController::bind_compact(CompactCallback compact) {
    const std::lock_guard lock{mutex_};
    compact_ = std::move(compact);
}

void MaintenanceController::bind_observe(ObserveCallback observe) {
    const std::lock_guard lock{mutex_};
    observe_ = std::move(observe);
}

void MaintenanceController::set_auto_compact_enabled(const bool enabled) noexcept {
    const std::lock_guard lock{mutex_};
    auto_compact_enabled_ = enabled;
}

void MaintenanceController::start() {
    if (config_.mode != MaintenanceMode::background) {
        const std::lock_guard lock{mutex_};
        state_ = MaintenanceState::stopped;
        return;
    }
    {
        const std::lock_guard lock{mutex_};
        if (worker_.joinable()) {
            return;
        }
        stop_requested_ = false;
        evaluate_requested_ = true; // first observation must not wait a mid-interval
        state_ = MaintenanceState::idle;
    }
    auto thread = std::thread([this] { run(); });
    {
        const std::lock_guard lock{mutex_};
        if (stop_requested_) {
            state_ = MaintenanceState::stopped;
        } else {
            worker_ = std::move(thread);
            wake_.notify_one();
            return;
        }
    }
    wake_.notify_all();
    thread.join();
}

void MaintenanceController::request_stop() noexcept {
    {
        const std::lock_guard lock{mutex_};
        stop_requested_ = true;
        if (state_ == MaintenanceState::compacting) {
            state_ = MaintenanceState::draining;
        } else if (state_ != MaintenanceState::faulted && state_ != MaintenanceState::draining) {
            state_ = MaintenanceState::stopped;
        }
        compact_ = {};
        observe_ = {};
        latency_guard_active_ = false;
        latency_deferral_started_.reset();
        clear_no_gain_backoff_locked();
        publish_mutations_rejected_locked(false);
    }
    wake_.notify_all();
}

void MaintenanceController::join() {
    std::thread local;
    {
        const std::lock_guard lock{mutex_};
        local = std::move(worker_);
    }
    if (local.joinable()) {
        local.join();
    }
    {
        const std::lock_guard lock{mutex_};
        publish_mutations_rejected_locked(false);
        if (state_ != MaintenanceState::faulted) {
            state_ = MaintenanceState::stopped;
        }
    }
}

void MaintenanceController::stop() {
    request_stop();
    join();
}

void MaintenanceController::request_evaluate() {
    {
        const std::lock_guard lock{mutex_};
        if (stop_requested_ || config_.mode != MaintenanceMode::background) {
            return;
        }
        evaluate_requested_ = true;
    }
    wake_.notify_one();
}

void MaintenanceController::report_foreground_latency(const std::uint64_t latency_ns) noexcept {
    if (config_.suspend_on_p99_latency_ms == 0) {
        return;
    }
    for (std::size_t index = 0; index < kForegroundLatencyBoundsNs.size(); ++index) {
        if (latency_ns <= kForegroundLatencyBoundsNs[index]) {
            foreground_latency_buckets_[index].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

auto MaintenanceController::consume_foreground_latency_window_locked() noexcept
    -> std::pair<std::uint64_t, std::uint64_t> {
    std::array<std::uint64_t, kForegroundLatencyBoundsNs.size()> counts{};
    std::uint64_t samples{};
    for (std::size_t index = 0; index < counts.size(); ++index) {
        counts[index] = foreground_latency_buckets_[index].exchange(0, std::memory_order_acq_rel);
        samples = counts[index] > std::numeric_limits<std::uint64_t>::max() - samples
                      ? std::numeric_limits<std::uint64_t>::max()
                      : samples + counts[index];
    }
    if (samples == 0) {
        return {0, 0};
    }
    const auto rank = samples - samples / 100U; // ceil(0.99 * samples), without overflow
    std::uint64_t cumulative{};
    for (std::size_t index = 0; index < counts.size(); ++index) {
        cumulative = counts[index] > std::numeric_limits<std::uint64_t>::max() - cumulative
                         ? std::numeric_limits<std::uint64_t>::max()
                         : cumulative + counts[index];
        if (cumulative >= rank) {
            return {samples, kForegroundLatencyBoundsNs[index]};
        }
    }
    return {samples, std::numeric_limits<std::uint64_t>::max()};
}

auto MaintenanceController::snapshot() const -> MaintenanceSnapshot {
    const std::lock_guard lock{mutex_};
    std::uint64_t since_useful = 0;
    if (last_useful_at_) {
        since_useful = elapsed_ns(*last_useful_at_);
    }
    const auto latency_deferral_age = latency_deferral_started_ ? elapsed_ns(*latency_deferral_started_) : 0;
    const auto snapshot_now = std::chrono::steady_clock::now();
    const auto no_gain_retry_after =
        no_gain_retry_at_ && snapshot_now < *no_gain_retry_at_
            ? static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(*no_gain_retry_at_ - snapshot_now)
                      .count())
            : 0;
    return MaintenanceSnapshot{
        .state = state_,
        .thread_running = worker_.joinable(),
        .mode = config_.mode,
        .pressure = pressure_,
        .last_activation_reason = last_activation_reason_,
        .mutations_rejected = mutations_rejected_.load(std::memory_order_relaxed),
        .evaluation_cycles = evaluation_cycles_,
        .compact_attempts = compact_attempts_,
        .compact_completed = compact_completed_,
        .useful_compactions = useful_compactions_,
        .sequence_conflicts = sequence_conflicts_,
        .skips = skips_,
        .suspend_count = suspend_count_,
        .consecutive_no_gain = consecutive_no_gain_,
        .no_gain_scans_suppressed = no_gain_scans_suppressed_,
        .no_gain_retry_after_ns = no_gain_retry_after,
        .bytes_copied_window = bytes_copied_window_,
        .total_bytes_copied = total_bytes_copied_,
        .last_bytes_copied = last_bytes_copied_,
        .last_records_copied = last_records_copied_,
        .last_expired_records_dropped = last_expired_records_dropped_,
        .total_expired_records_dropped = total_expired_records_dropped_,
        .last_no_gain_source_records_verified = last_no_gain_source_records_verified_,
        .last_no_gain_source_bytes_verified = last_no_gain_source_bytes_verified_,
        .last_no_gain_expired_records_dropped = last_no_gain_expired_records_dropped_,
        .total_no_gain_source_records_verified = total_no_gain_source_records_verified_,
        .total_no_gain_source_bytes_verified = total_no_gain_source_bytes_verified_,
        .total_no_gain_expired_records_dropped = total_no_gain_expired_records_dropped_,
        .last_eval_duration_ns = last_eval_duration_ns_,
        .last_compact_duration_ns = last_compact_duration_ns_,
        .last_compaction_pacing_delay_ns = last_compaction_pacing_delay_ns_,
        .total_compaction_pacing_delay_ns = total_compaction_pacing_delay_ns_,
        .last_compaction_pacing_sleep_count = last_compaction_pacing_sleep_count_,
        .last_compaction_pacing_burst_bytes = last_compaction_pacing_burst_bytes_,
        .ns_since_last_useful_compaction = since_useful,
        .rate_window_bytes_copied = rate_window_bytes_copied_,
        .rate_window_cpu_ns = rate_window_cpu_ns_,
        .foreground_latency_samples = foreground_latency_samples_,
        .last_foreground_p99_ns = last_foreground_p99_ns_,
        .latency_suspends = latency_suspends_,
        .latency_guard_active = latency_guard_active_,
        .latency_deferral_age_ns = latency_deferral_age,
        .latency_debt_overrides = latency_debt_overrides_,
        .last_skip_reason = last_skip_reason_,
        .last_observation = last_observation_,
        .last_error = last_error_,
    };
}

auto MaintenanceController::thread_running() const noexcept -> bool {
    const std::lock_guard lock{mutex_};
    return worker_.joinable();
}

auto MaintenanceController::mutations_rejected() const noexcept -> bool {
    return mutations_rejected_.load(std::memory_order_acquire);
}

auto MaintenanceController::compaction_copy_rate_limit() const noexcept -> std::uint64_t {
    const std::lock_guard lock{mutex_};
    return aggressive_pressure(pressure_) ? 0U : config_.max_copy_bytes_per_sec;
}

void MaintenanceController::publish_mutations_rejected(const bool rejected) noexcept {
    std::lock_guard lock{mutex_};
    publish_mutations_rejected_locked(rejected);
}

void MaintenanceController::publish_mutations_rejected_locked(const bool rejected) noexcept {
    // Once stop is requested, never re-arm the gate (in-flight eval must not undo request_stop).
    if (stop_requested_) {
        mutations_rejected_.store(false, std::memory_order_release);
        return;
    }
    mutations_rejected_.store(rejected, std::memory_order_release);
}

void MaintenanceController::refresh_rate_window_locked(
    const std::chrono::steady_clock::time_point now) noexcept {
    constexpr auto kWindow = std::chrono::seconds{1};
    if (rate_window_start_.time_since_epoch().count() == 0 || now - rate_window_start_ >= kWindow) {
        rate_window_start_ = now;
        rate_window_bytes_copied_ = 0;
        rate_window_cpu_ns_ = 0;
    }
}

auto MaintenanceController::remaining_copy_bytes_locked(const MaintenanceConfig& config) const noexcept
    -> std::optional<std::uint64_t> {
    if (config.max_copy_bytes_per_sec == 0) {
        return std::nullopt;
    }
    if (rate_window_bytes_copied_ >= config.max_copy_bytes_per_sec) {
        return 0;
    }
    return config.max_copy_bytes_per_sec - rate_window_bytes_copied_;
}

auto MaintenanceController::cpu_budget_exhausted_locked(const MaintenanceConfig& config) const noexcept
    -> bool {
    if (config.max_cpu_ms_per_window == 0) {
        return false;
    }
    const auto budget_ns = static_cast<std::uint64_t>(config.max_cpu_ms_per_window) * 1'000'000ULL;
    return rate_window_cpu_ns_ >= budget_ns;
}

auto MaintenanceController::same_no_gain_candidate(const MaintenanceObservation& left,
                                                   const MaintenanceObservation& right) noexcept -> bool {
    return left.durable == right.durable && left.sealed_segment_count == right.sealed_segment_count &&
           left.compaction_candidate_worker == right.compaction_candidate_worker &&
           left.candidate_sealed_record_bytes == right.candidate_sealed_record_bytes &&
           left.candidate_live_record_bytes == right.candidate_live_record_bytes &&
           left.candidate_dead_record_bytes == right.candidate_dead_record_bytes &&
           left.candidate_dead_byte_ratio_bp == right.candidate_dead_byte_ratio_bp &&
           left.candidate_scheduling_dead_byte_ratio_bp == right.candidate_scheduling_dead_byte_ratio_bp &&
           left.candidate_unread_expired_sealed_record_count ==
               right.candidate_unread_expired_sealed_record_count &&
           left.candidate_unread_expired_sealed_record_bytes ==
               right.candidate_unread_expired_sealed_record_bytes;
}

void MaintenanceController::clear_no_gain_backoff_locked() noexcept {
    consecutive_no_gain_ = 0;
    no_gain_candidate_.reset();
    no_gain_retry_at_.reset();
}

auto MaintenanceController::eval_interval_locked() const -> std::chrono::milliseconds {
    const bool under_pressure = aggressive_pressure(pressure_);
    const bool backoff = !under_pressure && consecutive_no_gain_ >= config_.max_no_gain_attempts &&
                         config_.max_no_gain_attempts > 0;
    return std::chrono::milliseconds{clamp_interval_ms(config_, under_pressure, backoff)};
}

void MaintenanceController::record_skip(const MaintenanceSkipReason reason, const MaintenanceState next,
                                        const MaintenanceActivationReason activation) {
    ++skips_;
    last_skip_reason_ = reason;
    last_activation_reason_ = activation;
    if (next == MaintenanceState::suspended) {
        ++suspend_count_;
    }
    state_ = stop_requested_ ? MaintenanceState::stopped : next;
}

void MaintenanceController::run() {
    for (;;) {
        {
            std::unique_lock lock{mutex_};
            if (stop_requested_) {
                break;
            }
            if (state_ != MaintenanceState::suspended && state_ != MaintenanceState::faulted) {
                state_ = MaintenanceState::idle;
            }
            const auto deadline = std::chrono::steady_clock::now() + eval_interval_locked();
            wake_.wait_until(lock, deadline, [&] { return stop_requested_ || evaluate_requested_; });
            evaluate_requested_ = false;
            if (stop_requested_) {
                break;
            }
        }
        evaluate_once();
    }
    {
        const std::lock_guard lock{mutex_};
        publish_mutations_rejected_locked(false);
        if (state_ != MaintenanceState::faulted) {
            state_ = MaintenanceState::stopped;
        }
    }
}

} // namespace glifistore
