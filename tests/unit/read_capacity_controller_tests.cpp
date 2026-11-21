#include "glifistore/server/read_capacity_controller.hpp"
#include "glifistore/server/read_capacity_manager.hpp"
#include "test.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

using namespace glifistore::server;

namespace {
auto observation(const std::uint64_t end_ms, const std::uint64_t demand = 800U) -> ReadCapacityObservation {
    ReadCapacityObservation result{.begin_ms = end_ms - 50U,
                                   .end_ms = end_ms,
                                   .demanded_gets_per_second = demand,
                                   .read_cpu_limited = true};
    result.profile.state = ReadCapacityState::qualified;
    result.profile.revision = 1;
    result.profile.valid_until_ms = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index <= kMaximumReadHelpers; ++index) {
        result.profile.gets_per_second[index] = 1000U + index * 500U;
    }
    return result;
}
} // namespace

GLIFI_TEST("read capacity unknown stale overlapping and changed profiles cannot manufacture votes") {
    ReadCapacityController controller{1, 2};
    std::array samples{observation(50)};
    samples[0].profile.state = ReadCapacityState::unknown;
    GLIFI_REQUIRE(controller.step(50, 2, samples));
    GLIFI_REQUIRE(controller.occupied() == 0);
    samples[0] = observation(100);
    GLIFI_REQUIRE(controller.step(100, 2, samples));
    GLIFI_REQUIRE(controller.step(150, 2, samples)); // Same window supplies no second vote.
    GLIFI_REQUIRE(controller.occupied() == 0);
    samples[0] = observation(200);
    samples[0].profile.revision = 2;
    GLIFI_REQUIRE(controller.step(200, 2, samples));
    GLIFI_REQUIRE(controller.occupied() == 0);
    samples[0] = observation(250);
    samples[0].profile.revision = 2;
    GLIFI_REQUIRE(controller.step(250, 2, samples));
    GLIFI_REQUIRE(controller.occupied() == 1);
    GLIFI_REQUIRE(!controller.step(249, 2, samples));
    samples[0].profile.state = ReadCapacityState::stale;
    GLIFI_REQUIRE(controller.step(300, 2, samples));
    GLIFI_REQUIRE(controller.allocations()[0].state == ReadHelperState::draining);
    GLIFI_REQUIRE(controller.occupied() == 1);
    GLIFI_REQUIRE(!controller.acknowledge_active(0, 300));
    GLIFI_REQUIRE(controller.acknowledge_parked(0));
    GLIFI_REQUIRE(!controller.acknowledge_parked(0));
}

GLIFI_TEST("read capacity counts arming and draining and checks counterfactual retirement") {
    ReadCapacityController controller{1, 1};
    for (const auto time : {50U, 100U}) {
        std::array samples{observation(time)};
        GLIFI_REQUIRE(controller.step(time, 1, samples));
    }
    GLIFI_REQUIRE(controller.occupied() == 1);
    GLIFI_REQUIRE(controller.allocations()[0].state == ReadHelperState::arming);
    GLIFI_REQUIRE(controller.acknowledge_active(0, 100));
    // 600/1500 is below .45, but 600/1000 is not: retiring would reheat the owner.
    for (std::uint64_t time = 150; time < 7000; time += 50) {
        std::array samples{observation(time, 600)};
        GLIFI_REQUIRE(controller.step(time, 1, samples));
        GLIFI_REQUIRE(controller.allocations()[0].state == ReadHelperState::active);
    }
    std::array samples{observation(7000, 800)};
    GLIFI_REQUIRE(controller.step(7000, 0, samples));
    GLIFI_REQUIRE(controller.allocations()[0].state == ReadHelperState::draining);
    GLIFI_REQUIRE(controller.occupied() == 1);
    samples[0] = observation(7050, 800);
    GLIFI_REQUIRE(controller.step(7050, 1, samples));
    GLIFI_REQUIRE(controller.occupied() == 1);
    GLIFI_REQUIRE(controller.acknowledge_parked(0));
}

GLIFI_TEST("read capacity gaps and unversioned profile edits cannot trigger expansion") {
    ReadCapacityController controller{1, 1};
    std::array samples{observation(50)};
    GLIFI_REQUIRE(controller.step(50, 1, samples));
    samples[0] = observation(500);
    GLIFI_REQUIRE(controller.step(500, 1, samples));
    GLIFI_REQUIRE(controller.occupied() == 0);
    samples[0] = observation(550);
    samples[0].profile.expand_per_mille = 600;
    GLIFI_REQUIRE(controller.step(550, 1, samples));
    GLIFI_REQUIRE(controller.occupied() == 0);
    samples[0] = observation(600);
    samples[0].profile.revision = 2;
    GLIFI_REQUIRE(controller.step(600, 1, samples));
    samples[0] = observation(650);
    samples[0].profile.revision = 2;
    GLIFI_REQUIRE(controller.step(650, 1, samples));
    GLIFI_REQUIRE(controller.occupied() == 1);
    GLIFI_REQUIRE(!controller.acknowledge_active(0, 700));
    GLIFI_REQUIRE(controller.acknowledge_active(0, 650));
}

GLIFI_TEST("read capacity manager applies and drains a synthetic qualified allocation") {
    auto profile = observation(50).profile;
    profile.valid_until_ms = 150;
    // Synthetic control fixture only: this is not a measured capacity qualification.
    ReadHelperConfig config{.maximum_helpers = 1, .cpu_limit = 4, .profiles = {profile}};
    auto manager = ReadCapacityManager::create(1, config);
    GLIFI_REQUIRE(manager.has_value());
    GLIFI_REQUIRE((*manager)->start().has_value());
    (*manager)->observe(0, observation(50), 0);
    GLIFI_REQUIRE((*manager)->stats().occupied == 0);
    (*manager)->observe(0, observation(100), 0);
    GLIFI_REQUIRE((*manager)->stats().active == 1);
    GLIFI_REQUIRE((*manager)->stats().grants == 1);
    (*manager)->observe(0, observation(200), 0);
    GLIFI_REQUIRE((*manager)->stats().occupied == 0);
    GLIFI_REQUIRE((*manager)->stats().retirements == 1);
    (*manager)->stop();
}

GLIFI_TEST("read capacity quiet retirement requires five seconds and no gain means no grant") {
    ReadCapacityController controller{1, 1};
    for (const auto time : {50U, 100U}) {
        std::array samples{observation(time)};
        GLIFI_REQUIRE(controller.step(time, 1, samples));
    }
    GLIFI_REQUIRE(controller.acknowledge_active(0, 100));
    for (std::uint64_t time = 150; time <= 5100; time += 50) {
        std::array samples{observation(time, 100)};
        GLIFI_REQUIRE(controller.step(time, 1, samples));
        GLIFI_REQUIRE(controller.allocations()[0].state == ReadHelperState::active);
    }
    std::array samples{observation(5150, 100)};
    GLIFI_REQUIRE(controller.step(5150, 1, samples));
    GLIFI_REQUIRE(controller.allocations()[0].state == ReadHelperState::draining);
    GLIFI_REQUIRE(controller.acknowledge_parked(0));
    for (std::uint64_t time = 5200; time < 6000; time += 50) {
        samples[0] = observation(time, std::numeric_limits<std::uint64_t>::max());
        samples[0].profile.gets_per_second.fill(1000);
        GLIFI_REQUIRE(controller.step(time, 1, samples));
        GLIFI_REQUIRE(controller.occupied() == 0);
    }
}

GLIFI_TEST("read capacity fractional thresholds never activate on zero demand") {
    ReadCapacityController controller{1, 1};
    for (std::uint64_t time = 50; time <= 200; time += 50) {
        std::array samples{observation(time, time <= 100 ? 0 : 1)};
        samples[0].profile.gets_per_second[0] = 1;
        samples[0].profile.gets_per_second[1] = 2;
        GLIFI_REQUIRE(controller.step(time, 1, samples));
        GLIFI_REQUIRE(controller.occupied() == (time == 200 ? 1U : 0U));
    }
    GLIFI_REQUIRE(controller.acknowledge_active(0, 200));
    for (std::uint64_t time = 250; time <= 5250; time += 50) {
        std::array samples{observation(time, 0)};
        samples[0].profile.gets_per_second[0] = 1;
        samples[0].profile.gets_per_second[1] = 2;
        GLIFI_REQUIRE(controller.step(time, 1, samples));
    }
    GLIFI_REQUIRE(controller.allocations()[0].state == ReadHelperState::draining);
}

GLIFI_TEST("read capacity equal weight fairness and replay preserve every hard credit") {
    ReadCapacityController first{3, 2};
    ReadCapacityController replay{3, 2};
    std::array<std::size_t, 3> grants{};
    std::uint64_t random = 17;
    for (std::uint64_t time = 50; time < 100'000; time += 50) {
        random = random * 6364136223846793005ULL + 1U;
        std::array samples{observation(time, 3000), observation(time, 3000), observation(time, 3000)};
        const auto target = time < 10'000 ? 1U : static_cast<unsigned>((random >> 32U) % 3U);
        GLIFI_REQUIRE(first.step(time, target, samples));
        GLIFI_REQUIRE(replay.step(time, target, samples));
        GLIFI_REQUIRE(first.occupied() <= 2);
        for (std::size_t index = 0; index < 2; ++index) {
            const auto a = first.allocations()[index];
            const auto b = replay.allocations()[index];
            GLIFI_REQUIRE(a.state == b.state && a.owner == b.owner && a.since_ms == b.since_ms);
            if (a.state == ReadHelperState::arming) {
                ++grants[a.owner];
                GLIFI_REQUIRE(first.acknowledge_active(index, time));
                GLIFI_REQUIRE(replay.acknowledge_active(index, time));
            } else if (a.state == ReadHelperState::draining && (random & 3U) == 0) {
                GLIFI_REQUIRE(first.acknowledge_parked(index));
                GLIFI_REQUIRE(replay.acknowledge_parked(index));
            }
        }
    }
    for (const auto count : grants) {
        GLIFI_REQUIRE(count > 5);
    }
    ReadCapacityController invalid{kMaximumReadOwners + 1U, 1};
    GLIFI_REQUIRE(!invalid.valid());
    GLIFI_REQUIRE(invalid.allocations().empty());
}
