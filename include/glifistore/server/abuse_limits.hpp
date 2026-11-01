#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace glifistore::server {

// Phase 5 abuse / DoS controls (security roadmap). Zero disables a limit.
// Secure-profile applies non-zero defaults and refuses explicit zero.
struct AbuseLimits final {
    // Process-wide accepted connection / handshake admissions per second.
    std::uint32_t max_accepts_per_sec{};
    // Close connections with no activity for this long (monotonic).
    std::uint32_t idle_timeout_ms{};
    // Bound partial-frame assembly, in-flight response wait, and stalled output
    // (time since first queue / last successful write, including close-after-flush).
    // If zero, idle_timeout_ms is the output-progress fallback. Both zero
    // explicitly disables output expiry for trusted deployments (ADR 0041).
    // Durable Store mutations already in execution are never cancelled; the TCP
    // peer is closed and the late response is discarded (stale generation).
    std::uint32_t request_timeout_ms{};
    // Per-connection request frames admitted per one-second window.
    std::uint32_t connection_max_requests_per_sec{};
    // Per-principal request frames admitted per one-second window (mTLS id).
    std::uint32_t principal_max_requests_per_sec{};
    // Per-principal request key+value + response value bytes per one-second window.
    std::uint64_t principal_max_bytes_per_sec{};

    [[nodiscard]] auto any_enabled() const noexcept -> bool {
        return max_accepts_per_sec != 0 || idle_timeout_ms != 0 || request_timeout_ms != 0 ||
               connection_max_requests_per_sec != 0 || principal_max_requests_per_sec != 0 ||
               principal_max_bytes_per_sec != 0;
    }
};

// Secure-profile defaults applied when operators leave Phase 5 knobs at zero.
[[nodiscard]] constexpr auto secure_profile_abuse_defaults() noexcept -> AbuseLimits {
    return AbuseLimits{
        .max_accepts_per_sec = 128,
        .idle_timeout_ms = 60'000,
        .request_timeout_ms = 30'000,
        .connection_max_requests_per_sec = 256,
        .principal_max_requests_per_sec = 1'024,
        .principal_max_bytes_per_sec = 32ULL * 1024ULL * 1024ULL,
    };
}

struct AbuseStats final {
    std::uint64_t accepts_rejected{};
    std::uint64_t idle_closed{};
    std::uint64_t request_timeout_closed{};
    std::uint64_t connection_rate_rejected{};
    std::uint64_t principal_request_rejected{};
    std::uint64_t principal_bandwidth_rejected{};
};

// Shared across Reactors so SO_REUSEPORT acceptors and principal quotas stay process-wide.
class AbuseController final {
  public:
    explicit AbuseController(AbuseLimits limits);

    [[nodiscard]] auto limits() const noexcept -> const AbuseLimits& {
        return limits_;
    }
    [[nodiscard]] auto enabled() const noexcept -> bool {
        return limits_.any_enabled();
    }

    // Returns false when the accept should be dropped (socket closed by caller).
    [[nodiscard]] auto try_admit_accept(std::chrono::steady_clock::time_point now) -> bool;

    // Returns false when the frame should be refused with overloaded (connection kept).
    [[nodiscard]] auto try_admit_connection_request(std::uint64_t& window_start_ns, std::uint32_t& used,
                                                    std::chrono::steady_clock::time_point now) -> bool;

    // Returns false when the principal is over request or bandwidth quota.
    // `bytes` counts request key+value size for this admission.
    [[nodiscard]] auto try_admit_principal(std::string_view principal, std::uint64_t bytes,
                                           std::chrono::steady_clock::time_point now) -> bool;

    void record_principal_response_bytes(std::string_view principal, std::uint64_t bytes,
                                         std::chrono::steady_clock::time_point now);

    void note_idle_closed() noexcept;
    void note_request_timeout_closed() noexcept;

    [[nodiscard]] auto stats() const noexcept -> AbuseStats;

  private:
    struct Window final {
        std::chrono::steady_clock::time_point start{};
        std::uint64_t used{};
    };

    struct PrincipalWindow final {
        Window requests{};
        Window bytes{};
    };

    [[nodiscard]] static auto try_consume(Window& window, std::uint64_t amount, std::uint64_t limit,
                                          std::chrono::steady_clock::time_point now) -> bool;

    AbuseLimits limits_;
    mutable std::mutex mutex_;
    Window accept_window_{};
    std::unordered_map<std::string, PrincipalWindow> principals_;
    std::atomic<std::uint64_t> accepts_rejected_{};
    std::atomic<std::uint64_t> idle_closed_{};
    std::atomic<std::uint64_t> request_timeout_closed_{};
    std::atomic<std::uint64_t> connection_rate_rejected_{};
    std::atomic<std::uint64_t> principal_request_rejected_{};
    std::atomic<std::uint64_t> principal_bandwidth_rejected_{};
};

} // namespace glifistore::server
