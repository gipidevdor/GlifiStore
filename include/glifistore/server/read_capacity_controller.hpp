/**
 * @file
 * @brief Bounded, deterministic read-capacity decisions with explicit drain acknowledgements.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace glifistore::server {

inline constexpr std::size_t kMaximumReadHelpers = 16;
inline constexpr std::size_t kMaximumReadOwners = 64;
inline constexpr std::size_t kNoReadOwner = std::numeric_limits<std::size_t>::max();

enum class ReadCapacityState : std::uint8_t { unknown, qualified, stale };
enum class ReadHelperState : std::uint8_t { parked, arming, active, draining };

/// Caller-supplied qualification, never inferred from observed completed throughput.
/// Capacities include the primary Reader. The identity binds workload, CPU allocation,
/// latency objective and shared-resource conditions; changing it resets pending votes.
struct ReadCapacityProfile final {
    ReadCapacityState state{ReadCapacityState::unknown};
    std::uint64_t revision{};
    std::uint64_t valid_until_ms{};
    std::array<std::uint64_t, kMaximumReadHelpers + 1U> gets_per_second{};
    std::uint16_t expand_per_mille{650};
    std::uint16_t retire_per_mille{450};
    auto operator==(const ReadCapacityProfile&) const -> bool = default;
};

/// One non-overlapping observation. Demand includes admitted work, not just completions.
/// read_cpu_limited excludes network, Writer barriers and cold-I/O waits.
struct ReadCapacityObservation final {
    std::uint64_t begin_ms{};
    std::uint64_t end_ms{};
    std::uint64_t demanded_gets_per_second{};
    bool read_cpu_limited{};
    ReadCapacityProfile profile{};
};

struct ReadHelperAllocation final {
    ReadHelperState state{ReadHelperState::parked};
    std::size_t owner{kNoReadOwner};
    std::uint64_t since_ms{};
};

/// Single control-thread state machine. No clocks, allocations, floating point, random
/// tie breaking or unordered iteration. A step grants at most one helper. Arming and
/// draining count against the hard cap until an explicit safe acknowledgement.
class ReadCapacityController final {
  public:
    ReadCapacityController(std::size_t owner_count, std::size_t helper_cap) noexcept;
    [[nodiscard]] auto valid() const noexcept -> bool;
    /// Rejects backwards/duplicate time or wrong owner count without changing state.
    /// target is a soft CPU budget; shrinking it does not release live allocations.
    [[nodiscard]] auto step(std::uint64_t now_ms, std::size_t target,
                            std::span<const ReadCapacityObservation> observations) noexcept -> bool;
    [[nodiscard]] auto acknowledge_active(std::size_t helper, std::uint64_t now_ms) noexcept -> bool;
    [[nodiscard]] auto acknowledge_parked(std::size_t helper) noexcept -> bool;
    [[nodiscard]] auto allocations() const noexcept -> std::span<const ReadHelperAllocation>;
    [[nodiscard]] auto occupied() const noexcept -> std::size_t;

  private:
    struct Owner final {
        std::uint64_t revision{};
        ReadCapacityProfile profile{};
        std::uint64_t last_end_ms{};
        std::uint64_t quiet_since_ms{};
        std::uint64_t settled_after_ms{};
        std::uint8_t hot_votes{};
        bool eligible{};
    };

    std::size_t owner_count_{};
    std::size_t helper_cap_{};
    std::size_t cursor_{};
    std::uint64_t last_step_ms_{};
    bool stepped_{};
    std::array<Owner, kMaximumReadOwners> owners_{};
    std::array<ReadHelperAllocation, kMaximumReadHelpers> helpers_{};
};

} // namespace glifistore::server
