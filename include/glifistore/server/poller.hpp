/**
 * @file
 * @brief Platform event polling with explicit readiness and token identity.
 */
#pragma once

#include "glifistore/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace glifistore::server {

/**
 * @brief Composable requested read/write poller interests.
 */
enum class IoInterest : std::uint8_t { none = 0, read = 1U << 0U, write = 1U << 1U };
/**
 * @brief Composable platform readiness/error observations; these flags do not validate token freshness.
 */
enum class IoFlags : std::uint8_t {
    none = 0,
    readable = 1U << 0U,
    writable = 1U << 1U,
    error = 1U << 2U,
    hangup = 1U << 3U
};

[[nodiscard]] constexpr auto operator|(const IoInterest left, const IoInterest right) noexcept -> IoInterest {
    return static_cast<IoInterest>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr auto has_interest(const IoInterest value, const IoInterest interest) noexcept
    -> bool {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(interest)) != 0;
}

[[nodiscard]] constexpr auto operator|(const IoFlags left, const IoFlags right) noexcept -> IoFlags {
    return static_cast<IoFlags>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr auto has_flag(const IoFlags value, const IoFlags flag) noexcept -> bool {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0;
}

/**
 * @brief Readiness/error flags associated with the exact token registered by the caller.
 */
struct IoEvent {
    std::uint64_t token{};
    IoFlags flags{IoFlags::none};
};

/**
 * @brief Move-only platform poller descriptor; registrations and wait are driven by its owner executor.
 */
class Poller final {
  public:
    Poller() = default;
    ~Poller();
    Poller(const Poller&) = delete;
    auto operator=(const Poller&) -> Poller& = delete;
    Poller(Poller&& other) noexcept;
    auto operator=(Poller&& other) noexcept -> Poller&;

    [[nodiscard]] static auto create() -> Result<Poller>;
    [[nodiscard]] auto add(int descriptor, std::uint64_t token, IoInterest interest) -> Status;
    [[nodiscard]] auto modify(int descriptor, std::uint64_t token, IoInterest interest) -> Status;
    [[nodiscard]] auto remove(int descriptor) -> Status;
    /**
     * @brief Fill at most events.size() readiness entries.
     * @param[out] events Caller-owned output storage.
     * @param timeout_ms Platform wait budget in milliseconds; -1 permits an unbounded wait.
     * @return Number of valid entries, zero on timeout, or a poller error.
     * @note Returned tokens must still be checked against current connection generations.
     */
    [[nodiscard]] auto wait(std::span<IoEvent> events, int timeout_ms) -> Result<std::size_t>;

  private:
    explicit Poller(int descriptor) noexcept : descriptor_(descriptor) {}

    [[nodiscard]] auto release() noexcept -> int;
    void reset(int descriptor = -1) noexcept;

    int descriptor_{-1};
};

} // namespace glifistore::server
