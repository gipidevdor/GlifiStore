#include "harness.hpp"
#include "owner_workload.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

GLIFI_TEST("owner workload preserves legacy bindings and uneven operation quotas") {
    const auto plan = glifistore::bench::owner_workload(3, 5, 12);
    GLIFI_REQUIRE(plan.has_value());
    GLIFI_REQUIRE(plan->client_owners == std::vector<std::size_t>({0, 1, 2, 0, 1}));
    GLIFI_REQUIRE(plan->operations_by_owner == std::vector<std::size_t>({5, 5, 2}));
}

GLIFI_TEST("owner workload concentrates nine clients and keeps every sibling represented") {
    const auto plan = glifistore::bench::owner_workload(4, 12, 120, 9, 2);
    GLIFI_REQUIRE(plan.has_value());
    GLIFI_REQUIRE(std::ranges::count(plan->client_owners, 2U) == 9);
    GLIFI_REQUIRE(plan->operations_by_owner == std::vector<std::size_t>({10, 10, 90, 10}));
    const auto remainder = glifistore::bench::owner_workload(2, 10, 103, 9);
    GLIFI_REQUIRE(remainder.has_value());
    GLIFI_REQUIRE(remainder->operations_by_owner == std::vector<std::size_t>({93, 10}));
}

GLIFI_TEST("owner workload rejects missing siblings and invalid coordinates") {
    using glifistore::bench::owner_workload;
    GLIFI_REQUIRE(!owner_workload(0, 1, 1));
    GLIFI_REQUIRE(!owner_workload(1, 0, 1));
    GLIFI_REQUIRE(!owner_workload(1, 1, 0));
    GLIFI_REQUIRE(!owner_workload(1, 2, 10, 1));
    GLIFI_REQUIRE(!owner_workload(2, 2, 10, 0));
    GLIFI_REQUIRE(!owner_workload(2, 2, 10, 2));
    GLIFI_REQUIRE(!owner_workload(4, 4, 10, 2));
    GLIFI_REQUIRE(!owner_workload(2, 4, 10, 2, 2));
    GLIFI_REQUIRE(!owner_workload(2, 4, 10, std::nullopt, 1));
}

GLIFI_TEST("owner workload accounts maximum operation counts without overflowing") {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    const auto plan = glifistore::bench::owner_workload(2, 3, maximum, 2);
    GLIFI_REQUIRE(plan.has_value());
    GLIFI_REQUIRE(plan->operations_by_owner[0] == maximum - maximum / 3U);
    GLIFI_REQUIRE(plan->operations_by_owner[1] == maximum / 3U);
}
