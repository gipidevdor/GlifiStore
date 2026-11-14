#include "glifistore/core/integer_math.hpp"
#include "glifistore/core/types.hpp"
#include "glifistore/store/maintenance.hpp"
#include "maintenance_detail.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace glifistore {
using maintenance_detail::aggressive_pressure;
using maintenance_detail::elapsed_ns;

void MaintenanceController::evaluate_once() {
    const auto eval_started = std::chrono::steady_clock::now();
    {
        const std::lock_guard lock{mutex_};
        if (stop_requested_) {
            return;
        }
        state_ = MaintenanceState::evaluating;
        ++evaluation_cycles_;
    }

    ObserveCallback observe;
    CompactCallback compact;
    bool auto_compact = false;
    MaintenanceConfig config{};
    {
        const std::lock_guard lock{mutex_};
        if (stop_requested_) {
            return;
        }
        observe = observe_;
        compact = compact_;
        auto_compact = auto_compact_enabled_;
        config = config_;
        bytes_copied_window_ = 0;
    }

    MaintenanceObservation observation{};
    if (!observe) {
        const std::lock_guard lock{mutex_};
        pressure_ = MaintenancePressureLevel::normal;
        publish_mutations_rejected_locked(false);
        if (!auto_compact || !compact) {
            last_eval_duration_ns_ = elapsed_ns(eval_started);
            record_skip(MaintenanceSkipReason::policy_deferred, MaintenanceState::idle,
                        MaintenanceActivationReason::policy_deferred);
            return;
        }
    } else {
        auto observed = observe({});
        if (!observed) {
            const std::lock_guard lock{mutex_};
            last_error_ = observed.error();
            last_eval_duration_ns_ = elapsed_ns(eval_started);
            if (observed.error().code == ErrorCode::unavailable) {
                publish_mutations_rejected_locked(false);
                record_skip(MaintenanceSkipReason::store_closed, MaintenanceState::idle,
                            MaintenanceActivationReason::none);
                return;
            }
            // Keep an already-published emergency gate. Do not latch auto-compact off while the
            // gate is armed: reclaim must keep retrying under emergency.
            state_ = MaintenanceState::faulted;
            if (!mutations_rejected_.load(std::memory_order_relaxed)) {
                auto_compact_enabled_ = false;
            }
            return;
        }
        observation = *observed;
        {
            const std::lock_guard lock{mutex_};
            last_observation_ = observation;
            pressure_ = classify_maintenance_pressure(observation, config_);
            publish_mutations_rejected_locked(pressure_ == MaintenancePressureLevel::emergency);
            if (stop_requested_) {
                return;
            }
        }
        const bool probe_unread_ttl =
            observe && observation.durable && observation.compaction_candidate_worker.has_value() &&
            ((config.unread_ttl_pressure_probe && aggressive_pressure(pressure_)) ||
             (config.unread_ttl_normal_scheduling && pressure_ == MaintenancePressureLevel::normal));
        if (probe_unread_ttl) {
            if (auto probed = observe(MaintenanceObserveRequest{.probe_unread_expired_ttl = true})) {
                observation.unread_ttl_probe_performed = probed->unread_ttl_probe_performed;
                observation.candidate_unread_expired_sealed_record_count =
                    probed->candidate_unread_expired_sealed_record_count;
                observation.candidate_unread_expired_sealed_record_bytes =
                    probed->candidate_unread_expired_sealed_record_bytes;
                const std::lock_guard lock{mutex_};
                last_observation_ = observation;
            } else {
                const std::lock_guard lock{mutex_};
                last_error_ = probed.error();
                last_eval_duration_ns_ = elapsed_ns(eval_started);
                if (probed.error().code == ErrorCode::unavailable) {
                    publish_mutations_rejected_locked(false);
                    record_skip(MaintenanceSkipReason::store_closed, MaintenanceState::idle,
                                MaintenanceActivationReason::none);
                    return;
                }
                state_ = MaintenanceState::faulted;
                const bool gate_armed = mutations_rejected_.load(std::memory_order_relaxed) ||
                                        pressure_ == MaintenancePressureLevel::emergency;
                if (!gate_armed) {
                    auto_compact_enabled_ = false;
                }
                return;
            }
        }
        observation.candidate_scheduling_dead_byte_ratio_bp = scheduling_dead_byte_ratio_bp(observation);
        {
            const std::lock_guard lock{mutex_};
            last_observation_.candidate_scheduling_dead_byte_ratio_bp =
                observation.candidate_scheduling_dead_byte_ratio_bp;
        }
        if (!auto_compact || !compact) {
            const std::lock_guard lock{mutex_};
            last_eval_duration_ns_ = elapsed_ns(eval_started);
            record_skip(MaintenanceSkipReason::policy_deferred, MaintenanceState::idle,
                        pressure_ == MaintenancePressureLevel::emergency
                            ? MaintenanceActivationReason::emergency_capacity
                            : MaintenanceActivationReason::policy_deferred);
            return;
        }
    }

    const auto pressure = [&] {
        const std::lock_guard lock{mutex_};
        return pressure_;
    }();
    const bool under_pressure = aggressive_pressure(pressure);
    std::uint64_t foreground_samples{};
    std::uint64_t foreground_p99_ns{};
    {
        const std::lock_guard lock{mutex_};
        const auto window = consume_foreground_latency_window_locked();
        foreground_samples = window.first;
        foreground_p99_ns = window.second;
        foreground_latency_samples_ = foreground_samples;
        last_foreground_p99_ns_ = foreground_p99_ns;
    }

    if (observation.durable && observation.sealed_segment_count == 0) {
        const std::lock_guard lock{mutex_};
        latency_guard_active_ = false;
        latency_deferral_started_.reset();
        clear_no_gain_backoff_locked();
        last_eval_duration_ns_ = elapsed_ns(eval_started);
        record_skip(MaintenanceSkipReason::no_candidate, MaintenanceState::idle,
                    pressure == MaintenancePressureLevel::emergency
                        ? MaintenanceActivationReason::emergency_capacity
                        : MaintenanceActivationReason::no_candidate);
        return;
    }

    if (!under_pressure && observation.durable) {
        auto scheduling_bp = observation.candidate_scheduling_dead_byte_ratio_bp;
        if (!scheduling_bp) {
            scheduling_bp = observation.candidate_dead_byte_ratio_bp;
        }
        if (scheduling_bp && *scheduling_bp < config.dead_byte_ratio_bp_normal) {
            const std::lock_guard lock{mutex_};
            latency_guard_active_ = false;
            latency_deferral_started_.reset();
            last_eval_duration_ns_ = elapsed_ns(eval_started);
            record_skip(MaintenanceSkipReason::reclaim_threshold, MaintenanceState::idle,
                        MaintenanceActivationReason::reclaim_threshold);
            return;
        }
    }

    bool latency_debt_override{};
    {
        const auto now = std::chrono::steady_clock::now();
        const std::lock_guard lock{mutex_};
        const bool enough_samples = foreground_samples >= config.suspend_on_p99_min_samples;
        if (under_pressure || config.suspend_on_p99_latency_ms == 0 || !enough_samples) {
            latency_guard_active_ = false;
            latency_deferral_started_.reset();
        } else {
            const auto suspend_ns =
                static_cast<std::uint64_t>(config.suspend_on_p99_latency_ms) * 1'000'000ULL;
            const auto resume_ns = std::max<std::uint64_t>(1'000'000ULL, suspend_ns - suspend_ns / 5U);
            if (!latency_guard_active_ && foreground_p99_ns >= suspend_ns) {
                latency_guard_active_ = true;
                latency_deferral_started_ = now;
            } else if (latency_guard_active_ && foreground_p99_ns < resume_ns) {
                latency_guard_active_ = false;
                latency_deferral_started_.reset();
            }

            if (latency_guard_active_) {
                const auto maximum_deferral_ns =
                    static_cast<std::uint64_t>(config.max_latency_deferral_ms) * 1'000'000ULL;
                const bool debt_due = maximum_deferral_ns != 0 && latency_deferral_started_ &&
                                      elapsed_ns(*latency_deferral_started_) >= maximum_deferral_ns;
                if (debt_due) {
                    latency_debt_override = true;
                    ++latency_debt_overrides_;
                    latency_guard_active_ = false;
                    latency_deferral_started_.reset();
                } else {
                    ++latency_suspends_;
                    last_eval_duration_ns_ = elapsed_ns(eval_started);
                    record_skip(MaintenanceSkipReason::latency_budget, MaintenanceState::suspended,
                                MaintenanceActivationReason::latency_budget);
                    return;
                }
            }
        }
    }

    // An exact no-gain result is deterministic while the selected Worker's
    // physical candidate is unchanged. Do not repeatedly rescan it. A changed
    // candidate invalidates immediately; the maximum evaluation interval
    // provides a bounded TTL/time-based retry. Capacity pressure always bypasses.
    {
        const auto now = std::chrono::steady_clock::now();
        const std::lock_guard lock{mutex_};
        if (no_gain_candidate_ && !same_no_gain_candidate(*no_gain_candidate_, observation)) {
            clear_no_gain_backoff_locked();
        }
        if (no_gain_candidate_ && no_gain_retry_at_ && now >= *no_gain_retry_at_) {
            clear_no_gain_backoff_locked();
        }
        if (!under_pressure && config.max_no_gain_attempts > 0 && no_gain_candidate_ && no_gain_retry_at_ &&
            now < *no_gain_retry_at_ && consecutive_no_gain_ >= config.max_no_gain_attempts) {
            ++no_gain_scans_suppressed_;
            last_eval_duration_ns_ = elapsed_ns(eval_started);
            record_skip(MaintenanceSkipReason::budget, MaintenanceState::suspended,
                        MaintenanceActivationReason::budget_backoff);
            return;
        }
    }

    // ADR 0044: a durable Worker larger than the per-cycle copy budget is no
    // longer skipped; the runtime compacts the oldest sealed prefix that fits.
    // `candidate_copy_bytes` bounds what this cycle can copy.
    std::uint64_t max_copy_bytes = under_pressure ? 0 : config.max_copy_bytes_per_cycle;
    const auto candidate_copy_bytes = max_copy_bytes == 0
                                          ? observation.candidate_live_record_bytes
                                          : std::min(observation.candidate_live_record_bytes, max_copy_bytes);
    if (!under_pressure) {
        const std::lock_guard lock{mutex_};
        refresh_rate_window_locked(std::chrono::steady_clock::now());
        if (cpu_budget_exhausted_locked(config)) {
            last_eval_duration_ns_ = elapsed_ns(eval_started);
            record_skip(MaintenanceSkipReason::rate_budget, MaintenanceState::suspended,
                        MaintenanceActivationReason::rate_budget);
            return;
        }
        if (const auto remaining = remaining_copy_bytes_locked(config)) {
            if (*remaining == 0) {
                last_eval_duration_ns_ = elapsed_ns(eval_started);
                record_skip(MaintenanceSkipReason::rate_budget, MaintenanceState::suspended,
                            MaintenanceActivationReason::rate_budget);
                return;
            }
            // The rate is enforced continuously by bounded pre-intent write
            // grants. Do not reinterpret the current-window remainder as a
            // whole-transaction copy cap: that would permanently starve every
            // candidate larger than one second of configured bandwidth.
            // A later candidate in an already-consumed window still waits for
            // refresh, preventing adjacent compactions from each taking a new
            // initial burst.
            if (rate_window_bytes_copied_ != 0U && observation.durable && candidate_copy_bytes > *remaining) {
                last_eval_duration_ns_ = elapsed_ns(eval_started);
                record_skip(MaintenanceSkipReason::rate_budget, MaintenanceState::suspended,
                            MaintenanceActivationReason::rate_budget);
                return;
            }
        }
    }

    MaintenanceActivationReason activation = MaintenanceActivationReason::scheduled;
    if (pressure == MaintenancePressureLevel::emergency) {
        activation = MaintenanceActivationReason::emergency_capacity;
    } else if (pressure == MaintenancePressureLevel::pressure) {
        if (observation.available_free_bytes.has_value() &&
            *observation.available_free_bytes <=
                saturating_add(observation.reserved_free_bytes, config.free_bytes_pressure_margin)) {
            activation = MaintenanceActivationReason::free_space_pressure;
        } else {
            activation = MaintenanceActivationReason::segment_pressure;
        }
    } else if (latency_debt_override) {
        activation = MaintenanceActivationReason::latency_debt_override;
    } else if (observation.durable && observation.sealed_segment_count > 0) {
        activation = MaintenanceActivationReason::sealed_history;
    }

    {
        const std::lock_guard lock{mutex_};
        if (stop_requested_ || !compact_) {
            return;
        }
        compact = compact_;
        state_ = MaintenanceState::compacting;
        last_activation_reason_ = activation;
        ++compact_attempts_;
    }

    const auto compact_started = std::chrono::steady_clock::now();
    auto result = compact(observation.compaction_candidate_worker, max_copy_bytes);
    const auto compact_ns = elapsed_ns(compact_started);
    {
        const std::lock_guard lock{mutex_};
        last_compact_duration_ns_ = compact_ns;
        last_eval_duration_ns_ = elapsed_ns(eval_started);
        refresh_rate_window_locked(std::chrono::steady_clock::now());
        if (!under_pressure) {
            rate_window_cpu_ns_ = compact_ns > std::numeric_limits<std::uint64_t>::max() - rate_window_cpu_ns_
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : rate_window_cpu_ns_ + compact_ns;
        }
        if (!result) {
            last_error_ = result.error();
            if (result.error().code == ErrorCode::unavailable) {
                publish_mutations_rejected_locked(false);
                record_skip(MaintenanceSkipReason::store_closed, MaintenanceState::idle,
                            MaintenanceActivationReason::none);
            } else if (result.error().code == ErrorCode::sequence_conflict) {
                ++sequence_conflicts_;
                record_skip(MaintenanceSkipReason::sequence_conflict, MaintenanceState::idle, activation);
            } else {
                // Preserve emergency rejection. While the gate is armed, keep auto-compact enabled
                // so budgeted reclaim retries continue (critical: do not wedge the Store).
                state_ = MaintenanceState::faulted;
                const bool gate_armed = mutations_rejected_.load(std::memory_order_relaxed) ||
                                        pressure_ == MaintenancePressureLevel::emergency;
                if (!gate_armed) {
                    auto_compact_enabled_ = false;
                }
            }
            return;
        }
        last_bytes_copied_ = result->bytes_copied;
        last_records_copied_ = result->records_copied;
        last_expired_records_dropped_ = result->expired_records_dropped;
        last_compaction_pacing_delay_ns_ = result->pacing_delay_ns;
        last_compaction_pacing_sleep_count_ = result->pacing_sleep_count;
        last_compaction_pacing_burst_bytes_ = result->pacing_burst_bytes;
        total_compaction_pacing_delay_ns_ =
            result->pacing_delay_ns >
                    std::numeric_limits<std::uint64_t>::max() - total_compaction_pacing_delay_ns_
                ? std::numeric_limits<std::uint64_t>::max()
                : total_compaction_pacing_delay_ns_ + result->pacing_delay_ns;
        if (!under_pressure) {
            rate_window_bytes_copied_ =
                result->bytes_copied > std::numeric_limits<std::uint64_t>::max() - rate_window_bytes_copied_
                    ? std::numeric_limits<std::uint64_t>::max()
                    : rate_window_bytes_copied_ + result->bytes_copied;
        }
        if (!result->compacted) {
            ++consecutive_no_gain_;
            last_no_gain_source_records_verified_ = result->source_records_verified;
            last_no_gain_source_bytes_verified_ = result->source_bytes_verified;
            last_no_gain_expired_records_dropped_ = result->expired_records_dropped;
            total_no_gain_source_records_verified_ += result->source_records_verified;
            total_no_gain_source_bytes_verified_ += result->source_bytes_verified;
            total_no_gain_expired_records_dropped_ += result->expired_records_dropped;
            if (config.max_no_gain_attempts > 0 && consecutive_no_gain_ >= config.max_no_gain_attempts) {
                no_gain_candidate_ = observation;
                no_gain_retry_at_ =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds{config.max_eval_interval_ms};
            }
            record_skip(MaintenanceSkipReason::no_gain, MaintenanceState::idle, activation);
            return;
        }
        ++compact_completed_;
        ++useful_compactions_;
        clear_no_gain_backoff_locked();
        bytes_copied_window_ = result->bytes_copied;
        total_bytes_copied_ += result->bytes_copied;
        total_expired_records_dropped_ += result->expired_records_dropped;
        last_useful_at_ = std::chrono::steady_clock::now();
        last_skip_reason_ = MaintenanceSkipReason::none;
        last_error_.reset();
        state_ = stop_requested_ ? MaintenanceState::stopped : MaintenanceState::idle;
    }
}

} // namespace glifistore
