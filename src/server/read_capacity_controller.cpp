#include "glifistore/server/read_capacity_controller.hpp"

#include <algorithm>
#include <limits>

namespace glifistore::server {
namespace {

// Round the threshold upward: integer demand must satisfy the exact fractional bound.
// Quotient/remainder avoids overflowing capacity * threshold for arbitrary traces.
[[nodiscard]] auto fraction(const std::uint64_t capacity, const std::uint16_t per_mille) noexcept
    -> std::uint64_t {
    return (capacity / 1000U) * per_mille + ((capacity % 1000U) * per_mille + 999U) / 1000U;
}

[[nodiscard]] auto qualified(const ReadCapacityProfile& profile, const std::uint64_t now_ms,
                             const std::size_t cap) noexcept -> bool {
    if (profile.state != ReadCapacityState::qualified || profile.revision == 0 ||
        profile.valid_until_ms < now_ms || profile.gets_per_second[0] == 0 ||
        profile.expand_per_mille > 1000U || profile.expand_per_mille < 200U ||
        profile.retire_per_mille > profile.expand_per_mille - 200U) {
        return false;
    }
    for (std::size_t index = 1; index <= cap; ++index) {
        if (profile.gets_per_second[index] < profile.gets_per_second[index - 1U]) {
            return false;
        }
    }
    return true;
}

} // namespace

ReadCapacityController::ReadCapacityController(const std::size_t owner_count,
                                               const std::size_t helper_cap) noexcept
    : owner_count_(owner_count), helper_cap_(helper_cap) {}

auto ReadCapacityController::valid() const noexcept -> bool {
    return owner_count_ > 0 && owner_count_ <= kMaximumReadOwners && helper_cap_ <= kMaximumReadHelpers;
}

auto ReadCapacityController::allocations() const noexcept -> std::span<const ReadHelperAllocation> {
    return {helpers_.data(), valid() ? helper_cap_ : 0U};
}

auto ReadCapacityController::occupied() const noexcept -> std::size_t {
    return static_cast<std::size_t>(
        std::count_if(allocations().begin(), allocations().end(),
                      [](const auto& helper) { return helper.state != ReadHelperState::parked; }));
}

auto ReadCapacityController::step(const std::uint64_t now_ms, const std::size_t target,
                                  const std::span<const ReadCapacityObservation> observations) noexcept
    -> bool {
    if (!valid() || observations.size() != owner_count_ || (stepped_ && now_ms <= last_step_ms_)) {
        return false;
    }
    stepped_ = true;
    last_step_ms_ = now_ms;
    std::array<std::size_t, kMaximumReadOwners> counts{};
    std::array<bool, kMaximumReadOwners> transitioning{};
    for (const auto& helper : allocations()) {
        if (helper.state != ReadHelperState::parked) {
            ++counts[helper.owner];
            transitioning[helper.owner] |= helper.state != ReadHelperState::active;
        }
    }
    for (std::size_t owner = 0; owner < owner_count_; ++owner) {
        auto& state = owners_[owner];
        const auto& observation = observations[owner];
        const auto& profile = observation.profile;
        const bool usable = qualified(profile, now_ms, helper_cap_) &&
                            (state.revision != profile.revision || state.profile == profile);
        if (!usable || state.revision != profile.revision) {
            state.hot_votes = 0;
            state.quiet_since_ms = 0;
            state.eligible = false;
            if (state.revision != profile.revision) {
                state.profile = profile;
            }
            state.revision = profile.revision;
        }
        if (!usable) {
            for (auto& helper : helpers_) {
                if (helper.owner == owner && helper.state != ReadHelperState::parked &&
                    helper.state != ReadHelperState::draining) {
                    helper.state = ReadHelperState::draining;
                    helper.since_ms = now_ms;
                }
            }
            continue;
        }
        if (observation.end_ms > now_ms || observation.begin_ms < state.last_end_ms ||
            observation.end_ms <= observation.begin_ms || observation.end_ms - observation.begin_ms < 50U ||
            observation.end_ms <= state.last_end_ms) {
            continue;
        }
        if (observation.begin_ms != state.last_end_ms) {
            state.hot_votes = 0;
            state.quiet_since_ms = 0;
        }
        state.last_end_ms = observation.end_ms;
        const auto count = counts[owner];
        const bool fresh = now_ms - observation.end_ms <= 100U;
        const bool hot = fresh && observation.read_cpu_limited && !transitioning[owner] &&
                         now_ms >= state.settled_after_ms && count < helper_cap_ &&
                         profile.gets_per_second[count + 1U] > profile.gets_per_second[count] &&
                         observation.demanded_gets_per_second >=
                             fraction(profile.gets_per_second[count], profile.expand_per_mille);
        state.hot_votes = hot ? static_cast<std::uint8_t>(std::min<unsigned>(2U, state.hot_votes + 1U)) : 0U;
        state.eligible = state.hot_votes >= 2U;
        // Project retirement onto C(k-1), not the lower utilization at C(k).
        const bool quiet = fresh && count != 0 && !transitioning[owner] &&
                           observation.demanded_gets_per_second <
                               fraction(profile.gets_per_second[count - 1U], profile.retire_per_mille);
        if (!quiet) {
            state.quiet_since_ms = 0;
        } else if (state.quiet_since_ms == 0) {
            state.quiet_since_ms = now_ms;
        } else if (now_ms - state.quiet_since_ms >= 5000U) {
            for (auto& helper : helpers_) {
                if (helper.owner == owner && helper.state == ReadHelperState::active &&
                    now_ms - helper.since_ms >= 500U) {
                    helper.state = ReadHelperState::draining;
                    helper.since_ms = now_ms;
                    state.quiet_since_ms = 0;
                    transitioning[owner] = true;
                    break;
                }
            }
        }
    }
    // Freshness expires eligibility even if the caller repeats an old observation.
    for (std::size_t owner = 0; owner < owner_count_; ++owner) {
        if (now_ms < owners_[owner].last_end_ms || now_ms - owners_[owner].last_end_ms > 100U) {
            owners_[owner].eligible = false;
            owners_[owner].hot_votes = 0;
        }
    }
    const auto budget = std::min(target, helper_cap_);
    auto retained = occupied();
    for (const auto& helper : allocations()) {
        retained -= helper.state == ReadHelperState::draining ? 1U : 0U;
    }
    for (std::size_t index = helper_cap_; index > 0 && retained > budget; --index) {
        auto& helper = helpers_[index - 1U];
        if (helper.state == ReadHelperState::active || helper.state == ReadHelperState::arming) {
            helper.state = ReadHelperState::draining;
            helper.since_ms = now_ms;
            --retained;
        }
    }
    std::size_t chosen = kNoReadOwner;
    for (std::size_t offset = 0; offset < owner_count_; ++offset) {
        const auto owner = (cursor_ + offset) % owner_count_;
        if (owners_[owner].eligible && !transitioning[owner] &&
            (chosen == kNoReadOwner || counts[owner] < counts[chosen])) {
            chosen = owner;
        }
    }
    if (chosen == kNoReadOwner || budget == 0) {
        return true;
    }
    if (occupied() < budget) {
        for (std::size_t index = 0; index < helper_cap_; ++index) {
            auto& helper = helpers_[index];
            if (helper.state == ReadHelperState::parked) {
                helper = {ReadHelperState::arming, chosen, now_ms};
                owners_[chosen].hot_votes = 0;
                owners_[chosen].eligible = false;
                cursor_ = (chosen + 1U) % owner_count_;
                break;
            }
        }
    } else {
        // Equal-share rotation only after a useful lease. Pending drain still occupies its slot.
        for (auto& helper : helpers_) {
            if (helper.state == ReadHelperState::active && helper.owner != chosen &&
                counts[helper.owner] >= counts[chosen] + 1U && now_ms - helper.since_ms >= 500U) {
                helper.state = ReadHelperState::draining;
                helper.since_ms = now_ms;
                break;
            }
        }
    }
    return true;
}

auto ReadCapacityController::acknowledge_active(const std::size_t helper, const std::uint64_t now_ms) noexcept
    -> bool {
    if (!valid() || helper >= helper_cap_ || helpers_[helper].state != ReadHelperState::arming ||
        now_ms < helpers_[helper].since_ms || now_ms > last_step_ms_) {
        return false;
    }
    auto& allocation = helpers_[helper];
    allocation.state = ReadHelperState::active;
    allocation.since_ms = now_ms;
    owners_[allocation.owner].settled_after_ms = now_ms > std::numeric_limits<std::uint64_t>::max() - 500U
                                                     ? std::numeric_limits<std::uint64_t>::max()
                                                     : now_ms + 500U;
    return true;
}

auto ReadCapacityController::acknowledge_parked(const std::size_t helper) noexcept -> bool {
    if (!valid() || helper >= helper_cap_ || helpers_[helper].state != ReadHelperState::draining) {
        return false;
    }
    helpers_[helper] = {};
    return true;
}

} // namespace glifistore::server
