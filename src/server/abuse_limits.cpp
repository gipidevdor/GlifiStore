#include "glifistore/server/abuse_limits.hpp"

namespace glifistore::server {
namespace {

constexpr auto kWindow = std::chrono::seconds{1};
constexpr std::size_t kMaxPrincipalEntries = 4096;

} // namespace

AbuseController::AbuseController(AbuseLimits limits) : limits_(limits) {}

auto AbuseController::try_consume(Window& window, const std::uint64_t amount, const std::uint64_t limit,
                                  const std::chrono::steady_clock::time_point now) -> bool {
    if (limit == 0) {
        return true;
    }
    if (window.start.time_since_epoch().count() == 0 || now - window.start >= kWindow) {
        window.start = now;
        window.used = 0;
    }
    if (amount > limit || window.used > limit - amount) {
        return false;
    }
    window.used += amount;
    return true;
}

auto AbuseController::try_admit_accept(const std::chrono::steady_clock::time_point now) -> bool {
    if (limits_.max_accepts_per_sec == 0) {
        return true;
    }
    const std::lock_guard lock{mutex_};
    if (!try_consume(accept_window_, 1, limits_.max_accepts_per_sec, now)) {
        accepts_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

auto AbuseController::try_admit_connection_request(std::uint64_t& window_start_ns, std::uint32_t& used,
                                                   const std::chrono::steady_clock::time_point now) -> bool {
    if (limits_.connection_max_requests_per_sec == 0) {
        return true;
    }
    const auto now_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    if (window_start_ns == 0 ||
        now_ns - window_start_ns >=
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(kWindow).count())) {
        window_start_ns = now_ns;
        used = 0;
    }
    if (used >= limits_.connection_max_requests_per_sec) {
        connection_rate_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ++used;
    return true;
}

auto AbuseController::try_admit_principal(const std::string_view principal, const std::uint64_t bytes,
                                          const std::chrono::steady_clock::time_point now) -> bool {
    if (limits_.principal_max_requests_per_sec == 0 && limits_.principal_max_bytes_per_sec == 0) {
        return true;
    }
    if (principal.empty()) {
        // Cleartext / anonymous peers are bounded by connection and accept limits only.
        return true;
    }
    const std::lock_guard lock{mutex_};
    auto found = principals_.find(std::string{principal});
    if (found == principals_.end()) {
        if (principals_.size() >= kMaxPrincipalEntries) {
            principal_request_rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        found = principals_.emplace(std::string{principal}, PrincipalWindow{}).first;
    }
    auto& entry = found->second;
    if (!try_consume(entry.requests, 1, limits_.principal_max_requests_per_sec, now)) {
        principal_request_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // A decided response may have exhausted the window. Even a zero-payload
    // INIT must then stop: it produces non-empty routing-identity output.
    const auto byte_limit = limits_.principal_max_bytes_per_sec;
    const bool byte_window_exhausted =
        byte_limit != 0 && entry.bytes.used >= byte_limit && now - entry.bytes.start < kWindow;
    if (byte_window_exhausted || !try_consume(entry.bytes, bytes, byte_limit, now)) {
        // Roll back the request token so bandwidth denial does not also burn request quota twice.
        if (entry.requests.used > 0) {
            --entry.requests.used;
        }
        principal_bandwidth_rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void AbuseController::record_principal_response_bytes(const std::string_view principal,
                                                      const std::uint64_t bytes,
                                                      const std::chrono::steady_clock::time_point now) {
    if (limits_.principal_max_bytes_per_sec == 0 || principal.empty() || bytes == 0) {
        return;
    }
    const std::lock_guard lock{mutex_};
    auto found = principals_.find(std::string{principal});
    if (found == principals_.end()) {
        if (principals_.size() >= kMaxPrincipalEntries) {
            return;
        }
        found = principals_.emplace(std::string{principal}, PrincipalWindow{}).first;
    }
    auto& window = found->second.bytes;
    if (!try_consume(window, bytes, limits_.principal_max_bytes_per_sec, now)) {
        // Output is already decided. Preserve its ACK/value but exhaust this
        // window so oversized responses cannot repeatedly escape accounting.
        window.used = limits_.principal_max_bytes_per_sec;
    }
}

void AbuseController::note_idle_closed() noexcept {
    idle_closed_.fetch_add(1, std::memory_order_relaxed);
}

void AbuseController::note_request_timeout_closed() noexcept {
    request_timeout_closed_.fetch_add(1, std::memory_order_relaxed);
}

auto AbuseController::stats() const noexcept -> AbuseStats {
    return AbuseStats{
        .accepts_rejected = accepts_rejected_.load(std::memory_order_relaxed),
        .idle_closed = idle_closed_.load(std::memory_order_relaxed),
        .request_timeout_closed = request_timeout_closed_.load(std::memory_order_relaxed),
        .connection_rate_rejected = connection_rate_rejected_.load(std::memory_order_relaxed),
        .principal_request_rejected = principal_request_rejected_.load(std::memory_order_relaxed),
        .principal_bandwidth_rejected = principal_bandwidth_rejected_.load(std::memory_order_relaxed),
    };
}

} // namespace glifistore::server
