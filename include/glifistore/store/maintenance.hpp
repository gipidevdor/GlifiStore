/**
 * @file
 * @brief Bounded maintenance scheduling from pressure, latency, and reclaim observations.
 */
#pragma once

#include "glifistore/store/maintenance_types.hpp"
#include "glifistore/store/store.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace glifistore {

/// Store-owned optional scheduler. Phase 3: normal + pressure + emergency mutation gate.
class MaintenanceController final {
  public:
    using CompactCallback = std::function<Result<CompactionResult>(
        std::optional<std::size_t> preferred_worker, std::uint64_t max_copy_bytes)>;
    using ObserveCallback = std::function<Result<MaintenanceObservation>(MaintenanceObserveRequest)>;

    explicit MaintenanceController(MaintenanceConfig config);
    ~MaintenanceController();

    MaintenanceController(const MaintenanceController&) = delete;
    auto operator=(const MaintenanceController&) -> MaintenanceController& = delete;

    void bind_compact(CompactCallback compact);
    void bind_observe(ObserveCallback observe);
    void set_auto_compact_enabled(bool enabled) noexcept;
    void start();
    void request_stop() noexcept;
    void join();
    void stop();
    void request_evaluate();
    /// Lock-free daemon feedback. A background evaluation consumes the fixed
    /// histogram window; embedded users that do not report samples are
    /// unaffected.
    void report_foreground_latency(std::uint64_t latency_ns) noexcept;

    [[nodiscard]] auto snapshot() const -> MaintenanceSnapshot;
    [[nodiscard]] auto thread_running() const noexcept -> bool;
    /// Lock-free admission probe for Store::put/erase (memory_order_acquire).
    [[nodiscard]] auto mutations_rejected() const noexcept -> bool;
    /// Synchronous compact callback query. Normal maintenance returns its
    /// configured byte rate; pressure/emergency return zero to preserve reclaim
    /// progress. The value is sampled once before the pre-intent build.
    [[nodiscard]] auto compaction_copy_rate_limit() const noexcept -> std::uint64_t;
    /// Publish or clear the emergency mutation gate (eval path + litmus TOCTOU).
    void publish_mutations_rejected(bool rejected) noexcept;

  private:
    void run();
    [[nodiscard]] auto eval_interval_locked() const -> std::chrono::milliseconds;
    void evaluate_once();
    void record_skip(MaintenanceSkipReason reason, MaintenanceState next,
                     MaintenanceActivationReason activation);
    void publish_mutations_rejected_locked(bool rejected) noexcept;
    void refresh_rate_window_locked(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] auto remaining_copy_bytes_locked(const MaintenanceConfig& config) const noexcept
        -> std::optional<std::uint64_t>;
    [[nodiscard]] auto cpu_budget_exhausted_locked(const MaintenanceConfig& config) const noexcept -> bool;
    [[nodiscard]] auto consume_foreground_latency_window_locked() noexcept
        -> std::pair<std::uint64_t, std::uint64_t>;
    [[nodiscard]] static auto same_no_gain_candidate(const MaintenanceObservation& left,
                                                     const MaintenanceObservation& right) noexcept -> bool;
    void clear_no_gain_backoff_locked() noexcept;

    MaintenanceConfig config_;
    CompactCallback compact_;
    ObserveCallback observe_;
    bool auto_compact_enabled_{true};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_requested_{};
    bool evaluate_requested_{};
    MaintenanceState state_{MaintenanceState::stopped};
    MaintenancePressureLevel pressure_{MaintenancePressureLevel::none};
    MaintenanceActivationReason last_activation_reason_{MaintenanceActivationReason::none};
    std::atomic<bool> mutations_rejected_{false};
    std::uint64_t evaluation_cycles_{};
    std::uint64_t compact_attempts_{};
    std::uint64_t compact_completed_{};
    std::uint64_t useful_compactions_{};
    std::uint64_t sequence_conflicts_{};
    std::uint64_t skips_{};
    std::uint64_t suspend_count_{};
    std::uint64_t consecutive_no_gain_{};
    std::uint64_t no_gain_scans_suppressed_{};
    std::optional<MaintenanceObservation> no_gain_candidate_{};
    std::optional<std::chrono::steady_clock::time_point> no_gain_retry_at_{};
    std::uint64_t bytes_copied_window_{};
    std::uint64_t total_bytes_copied_{};
    std::uint64_t last_bytes_copied_{};
    std::uint64_t last_records_copied_{};
    std::uint64_t last_expired_records_dropped_{};
    std::uint64_t total_expired_records_dropped_{};
    std::uint64_t last_no_gain_source_records_verified_{};
    std::uint64_t last_no_gain_source_bytes_verified_{};
    std::uint64_t last_no_gain_expired_records_dropped_{};
    std::uint64_t total_no_gain_source_records_verified_{};
    std::uint64_t total_no_gain_source_bytes_verified_{};
    std::uint64_t total_no_gain_expired_records_dropped_{};
    std::uint64_t last_eval_duration_ns_{};
    std::uint64_t last_compact_duration_ns_{};
    std::uint64_t last_compaction_pacing_delay_ns_{};
    std::uint64_t total_compaction_pacing_delay_ns_{};
    std::uint64_t last_compaction_pacing_sleep_count_{};
    std::uint64_t last_compaction_pacing_burst_bytes_{};
    std::optional<std::chrono::steady_clock::time_point> last_useful_at_{};
    std::chrono::steady_clock::time_point rate_window_start_{};
    std::uint64_t rate_window_bytes_copied_{};
    std::uint64_t rate_window_cpu_ns_{};
    static constexpr std::array<std::uint64_t, 9> kForegroundLatencyBoundsNs{
        1'000'000ULL,   5'000'000ULL,     10'000'000ULL,
        25'000'000ULL,  50'000'000ULL,    100'000'000ULL,
        250'000'000ULL, 1'000'000'000ULL, std::numeric_limits<std::uint64_t>::max()};
    std::array<std::atomic_uint64_t, kForegroundLatencyBoundsNs.size()> foreground_latency_buckets_{};
    std::uint64_t foreground_latency_samples_{};
    std::uint64_t last_foreground_p99_ns_{};
    std::uint64_t latency_suspends_{};
    bool latency_guard_active_{};
    std::optional<std::chrono::steady_clock::time_point> latency_deferral_started_{};
    std::uint64_t latency_debt_overrides_{};
    MaintenanceSkipReason last_skip_reason_{MaintenanceSkipReason::none};
    MaintenanceObservation last_observation_{};
    std::optional<Error> last_error_{};
    std::thread worker_;
};

} // namespace glifistore
